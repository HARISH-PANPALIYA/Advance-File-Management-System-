use anyhow::{Context, Result};
use rayon::prelude::*;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Default, Deserialize, Serialize)]
pub struct SearchSchema {
    #[serde(default)]
    pub keywords: Vec<String>,
    #[serde(default)]
    pub extensions: Vec<String>,
    #[serde(default)]
    pub intent: String,
}

#[derive(Debug, Serialize)]
pub struct SearchResult {
    pub path: PathBuf,
    pub file_name: String,
    pub extension: Option<String>,
    pub size_bytes: u64,
}

pub fn parse_query(query: &str) -> Result<SearchSchema> {
    if let Ok(schema) = serde_json::from_str(query) {
        return Ok(schema);
    }
    Ok(SearchSchema {
        keywords: query
            .split_whitespace()
            .map(|word| word.trim_matches(|character: char| !character.is_alphanumeric()))
            .filter(|word| !word.is_empty())
            .map(str::to_lowercase)
            .collect(),
        ..SearchSchema::default()
    })
}

pub fn search(root: &Path, schema: &SearchSchema) -> Result<Vec<SearchResult>> {
    let entries = jwalk::WalkDir::new(root)
        .follow_links(false)
        .into_iter()
        .filter_map(Result::ok)
        .filter(|entry| entry.file_type().is_file())
        .collect::<Vec<_>>();

    Ok(entries
        .par_iter()
        .filter_map(|entry| {
            let path = entry.path();
            let metadata = std::fs::metadata(&path).ok()?;
            let file_name = path.file_name()?.to_string_lossy().into_owned();
            let extension = path
                .extension()
                .map(|value| value.to_string_lossy().to_lowercase());
            if !matches_extension(extension.as_deref(), &schema.extensions)
                || !matches_keywords(&file_name, schema)
            {
                return None;
            }
            Some(SearchResult {
                path: path.to_path_buf(),
                file_name,
                extension,
                size_bytes: metadata.len(),
            })
        })
        .collect())
}

pub fn parse_with_groq(query: &str, api_key: &str) -> Result<SearchSchema> {
    let response = reqwest::blocking::Client::new()
        .post("https://api.groq.com/openai/v1/chat/completions")
        .bearer_auth(api_key)
        .json(&serde_json::json!({
            "model": "llama-3.1-8b-instant",
            "temperature": 0,
            "response_format": { "type": "json_object" },
            "messages": [
                {"role": "system", "content": "Return JSON only with keywords (array of strings), extensions (array of strings), and intent (string)."},
                {"role": "user", "content": query}
            ]
        }))
        .send()
        .context("send Groq search request")?
        .error_for_status()
        .context("Groq returned an error")?
        .json::<serde_json::Value>()
        .context("decode Groq response")?;
    let content = response["choices"][0]["message"]["content"]
        .as_str()
        .context("Groq response did not contain JSON content")?;
    serde_json::from_str(content).context("parse Groq search schema")
}

fn matches_extension(extension: Option<&str>, requested: &[String]) -> bool {
    requested.is_empty()
        || extension.is_some_and(|value| {
            requested
                .iter()
                .any(|item| item.trim_start_matches('.').eq_ignore_ascii_case(value))
        })
}

fn matches_keywords(file_name: &str, schema: &SearchSchema) -> bool {
    let lower_name = file_name.to_lowercase();
    schema.keywords.is_empty()
        || schema
            .keywords
            .iter()
            .any(|keyword| lower_name.contains(&keyword.to_lowercase()))
}
