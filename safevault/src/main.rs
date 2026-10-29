mod journal;
mod passthrough;
mod search;
mod watch;

use anyhow::Result;
use axum::{extract::State, http::StatusCode, routing::post, Json, Router};
use clap::{Parser, Subcommand};
use journal::{EventKind, EventRecord, Journal};
use search::{parse_query, parse_with_groq, search, SearchSchema};
use std::path::PathBuf;
use tower_http::cors::CorsLayer;

#[derive(Debug, Parser)]
#[command(name = "safevault", about = "SafeVault FS journaling foundation")]
struct Cli {
    #[arg(long, default_value = ".safevault_storage/safevault.sqlite3")]
    database: PathBuf,
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    Init,
    Record {
        #[arg(value_enum)]
        event: EventKind,
        path: PathBuf,
        #[arg(long)]
        old_path: Option<PathBuf>,
    },
    History {
        #[arg(long)]
        path: Option<PathBuf>,
    },
    Search {
        query: String,
        #[arg(long, default_value = ".")]
        root: PathBuf,
    },
    Serve {
        #[arg(long, default_value = "127.0.0.1:8787")]
        bind: String,
    },
    Watch {
        root: PathBuf,
    },
}

fn main() -> Result<()> {
    dotenvy::dotenv()
        .or_else(|_| dotenvy::from_filename("../.env"))
        .ok();
    let cli = Cli::parse();
    let journal = Journal::open(&cli.database)?;

    match cli.command {
        Command::Init => println!("Initialized {}", cli.database.display()),
        Command::Record {
            event,
            path,
            old_path,
        } => {
            let id = journal.append(EventRecord::now(event, path, old_path, std::process::id()))?;
            println!("Recorded event {id}");
        }
        Command::History { path } => {
            for event in journal.history(path.as_deref())? {
                println!(
                    "#{:04} {} {} pid={}{}",
                    event.id,
                    event.timestamp,
                    event.kind,
                    event.pid,
                    event
                        .old_path
                        .map(|old| format!(" (from {})", old.display()))
                        .unwrap_or_default()
                );
                println!("      {}", event.path.display());
            }
        }
        Command::Search { query, root } => {
            let schema = ai_schema(&query)?;
            for result in search(&root, &schema)? {
                println!("{}", serde_json::to_string(&result)?);
            }
        }
        Command::Serve { bind } => run_server(&bind)?,
        Command::Watch { root } => watch::watch(root, cli.database.clone())?,
    }
    Ok(())
}

fn ai_schema(query: &str) -> Result<SearchSchema> {
    match std::env::var("GROQ_API_KEY") {
        Ok(api_key) if !api_key.trim().is_empty() => parse_with_groq(query, &api_key),
        _ => parse_query(query),
    }
}

#[derive(Clone)]
struct ApiState {
    groq_api_key: Option<String>,
}

#[derive(serde::Deserialize)]
struct ParseRequest {
    query: String,
}

async fn parse_endpoint(
    State(state): State<ApiState>,
    Json(request): Json<ParseRequest>,
) -> Result<Json<SearchSchema>, (StatusCode, String)> {
    let schema = match state.groq_api_key {
        Some(api_key) if !api_key.trim().is_empty() => parse_with_groq(&request.query, &api_key),
        _ => parse_query(&request.query),
    }
    .map_err(|error| (StatusCode::BAD_GATEWAY, error.to_string()))?;
    Ok(Json(schema))
}

fn run_server(bind: &str) -> Result<()> {
    let state = ApiState {
        groq_api_key: std::env::var("GROQ_API_KEY").ok(),
    };
    let router = Router::new()
        .route("/api/parse", post(parse_endpoint))
        .layer(CorsLayer::permissive())
        .with_state(state);
    let listener = std::net::TcpListener::bind(bind)?;
    listener.set_nonblocking(true)?;
    let runtime = tokio::runtime::Runtime::new()?;
    runtime.block_on(async {
        let listener = tokio::net::TcpListener::from_std(listener)?;
        axum::serve(listener, router).await?;
        Ok::<(), anyhow::Error>(())
    })?;
    Ok(())
}
