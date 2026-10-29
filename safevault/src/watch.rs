use anyhow::{Context, Result};
use notify::{Config, Event, EventKind as NotifyKind, RecommendedWatcher, RecursiveMode, Watcher};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver};
use std::time::Duration;

use crate::journal::{EventKind, EventRecord, Journal};

pub fn watch(root: PathBuf, database: PathBuf) -> Result<()> {
    let journal = Journal::open(&database)?;
    let (sender, receiver) = mpsc::channel();
    let mut watcher = RecommendedWatcher::new(
        sender,
        Config::default().with_poll_interval(Duration::from_millis(500)),
    )
    .context("create filesystem watcher")?;
    watcher
        .watch(&root, RecursiveMode::Recursive)
        .with_context(|| format!("watch {}", root.display()))?;
    println!(
        "Watching {}. The menu is active; changes are journaled immediately.",
        root.display()
    );
    let snapshots = initial_snapshots(&root);
    std::thread::spawn(move || journal_loop(receiver, journal, snapshots));
    let menu_journal = Journal::open(&database)?;
    menu_loop(menu_journal, root)
}

fn menu_loop(journal: Journal, root: PathBuf) -> Result<()> {
    loop {
        println!("\nSafeVault menu");
        println!("1. Show recent activity");
        println!("2. Show file history");
        println!("3. Search files");
        println!("4. List saved versions");
        println!("5. Roll back a file");
        println!("6. Keep watching (refresh)");
        println!("7. Stop watcher");
        let mut choice = String::new();
        std::io::stdin().read_line(&mut choice)?;
        match choice.trim() {
            "1" => print_history(&journal, &root, None)?,
            "2" => {
                println!("Enter a full file path, or press Enter for all files:");
                let mut path = String::new();
                std::io::stdin().read_line(&mut path)?;
                let path = normalize_path(path.trim());
                print_history(
                    &journal,
                    &root,
                    (!path.is_empty()).then_some(Path::new(&path)),
                )?;
            }
            "3" => println!(
                "Use the separate command: cargo run -- search \"your query\" --root \"{}\"",
                root.display()
            ),
            "4" => list_versions(&journal)?,
            "5" => rollback(&journal)?,
            "6" => println!("Watching..."),
            "7" => {
                println!("Watcher stopped.");
                return Ok(());
            }
            _ => println!("Choose an option from 1 to 7."),
        }
    }
}

fn initial_snapshots(root: &Path) -> HashMap<PathBuf, Vec<u8>> {
    let mut snapshots = HashMap::new();
    let entries = match walkdir::WalkDir::new(root)
        .into_iter()
        .collect::<Result<Vec<_>, _>>()
    {
        Ok(entries) => entries,
        Err(_) => return snapshots,
    };
    for entry in entries
        .into_iter()
        .filter(|entry| entry.file_type().is_file())
    {
        if let Ok(content) = fs::read(entry.path()) {
            snapshots.insert(entry.path().to_path_buf(), content);
        }
    }
    snapshots
}

fn journal_loop(
    receiver: Receiver<notify::Result<Event>>,
    journal: Journal,
    mut snapshots: HashMap<PathBuf, Vec<u8>>,
) {
    while let Ok(result) = receiver.recv() {
        let event = match result {
            Ok(event) => event,
            Err(error) => {
                eprintln!("Filesystem watcher error: {error}");
                continue;
            }
        };
        let kind = match event.kind {
            NotifyKind::Create(_) => Some(EventKind::Create),
            NotifyKind::Modify(_) => Some(EventKind::Modify),
            NotifyKind::Remove(_) => Some(EventKind::Delete),
            _ => None,
        };
        if let Some(kind) = kind {
            for path in event.paths {
                if matches!(kind, EventKind::Modify | EventKind::Delete) {
                    if let Some(previous) = snapshots.get(&path) {
                        if let Err(error) = journal.save_version(&path, previous) {
                            eprintln!("Could not save version {}: {error}", path.display());
                        }
                    }
                }
                if let Err(error) = journal.append(EventRecord::now(
                    kind,
                    path.clone(),
                    None,
                    std::process::id(),
                )) {
                    eprintln!("Could not journal {}: {error}", path.display());
                    continue;
                }
                if kind != EventKind::Delete {
                    if let Ok(content) = fs::read(&path) {
                        snapshots.insert(path.clone(), content);
                    }
                } else {
                    snapshots.remove(&path);
                }
                println!("[{}] {}", kind.label(), path.display());
            }
        }
    }
}

fn list_versions(journal: &Journal) -> Result<()> {
    println!("Enter the full file path:");
    let mut path = String::new();
    std::io::stdin().read_line(&mut path)?;
    let path = PathBuf::from(normalize_path(path.trim()));
    let versions = journal.versions(&path)?;
    if versions.is_empty() {
        println!("No saved versions for {}", path.display());
    } else {
        println!("Version ID is the number after #. PID values in activity are not version IDs.");
        for version in versions {
            println!(
                "#{} {} {} bytes",
                version.id, version.created_at, version.size_bytes
            );
        }
    }
    Ok(())
}

fn rollback(journal: &Journal) -> Result<()> {
    println!("Enter the full file path to roll back:");
    let mut path = String::new();
    std::io::stdin().read_line(&mut path)?;
    let path = PathBuf::from(normalize_path(path.trim()));
    let versions = journal.versions(&path)?;
    if versions.is_empty() {
        println!(
            "No saved versions for {}. Modify the file while the watcher is running first.",
            path.display()
        );
        return Ok(());
    }
    println!("Available versions (use the number after #):");
    for version in &versions {
        println!(
            "#{} {} {} bytes",
            version.id, version.created_at, version.size_bytes
        );
    }
    println!("Enter the saved version ID:");
    let mut id = String::new();
    std::io::stdin().read_line(&mut id)?;
    let id: i64 = match id.trim().parse() {
        Ok(id) => id,
        Err(_) => {
            println!("Invalid version ID. Use a number such as 1 or 2.");
            return Ok(());
        }
    };
    match journal.version_content(id) {
        Ok((stored_path, content))
            if normalize_path(&stored_path.to_string_lossy())
                .eq_ignore_ascii_case(&path.to_string_lossy()) =>
        {
            fs::write(&stored_path, content)?;
            println!("Restored version #{} to {}", id, stored_path.display());
        }
        Ok(_) => println!("That version belongs to a different file. Choose an ID listed above."),
        Err(_) => println!("Version #{} was not found. Choose an ID listed above.", id),
    }
    Ok(())
}

fn normalize_path(value: &str) -> String {
    value.trim().trim_matches('"').trim_matches('\'').to_owned()
}

fn print_history(journal: &Journal, root: &Path, path: Option<&Path>) -> Result<()> {
    let events = match path {
        Some(path) => journal.history(Some(path))?,
        None => journal.history_under(root)?,
    };
    if events.is_empty() {
        println!("No journal events recorded yet.");
    } else {
        for event in events.iter().rev().take(50).rev() {
            println!(
                "#{:04} {} {} pid={} {}",
                event.id,
                event.timestamp,
                event.kind,
                event.pid,
                event.path.display()
            );
        }
    }
    Ok(())
}
