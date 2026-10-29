use anyhow::Result;
use chrono::Utc;
use clap::ValueEnum;
use rusqlite::{params, Connection};
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, ValueEnum)]
pub enum EventKind {
    Create,
    Modify,
    Delete,
    Rename,
}

impl EventKind {
    fn as_str(self) -> &'static str {
        match self {
            Self::Create => "CREATE",
            Self::Modify => "MODIFY",
            Self::Delete => "DELETE",
            Self::Rename => "RENAME",
        }
    }

    pub fn label(self) -> &'static str {
        self.as_str()
    }
}

#[derive(Debug)]
pub struct EventRecord {
    pub id: i64,
    pub kind: String,
    pub path: PathBuf,
    pub old_path: Option<PathBuf>,
    pub timestamp: String,
    pub pid: u32,
}

#[derive(Debug)]
pub struct VersionRecord {
    pub id: i64,
    pub path: PathBuf,
    pub created_at: String,
    pub size_bytes: usize,
}

impl EventRecord {
    pub fn now(kind: EventKind, path: PathBuf, old_path: Option<PathBuf>, pid: u32) -> Self {
        Self {
            id: 0,
            kind: kind.as_str().to_owned(),
            path,
            old_path,
            timestamp: Utc::now().to_rfc3339(),
            pid,
        }
    }
}

pub struct Journal {
    connection: Connection,
}

impl Journal {
    pub fn open(database_path: &Path) -> Result<Self> {
        if let Some(parent) = database_path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let connection = Connection::open(database_path)?;
        connection.pragma_update(None, "journal_mode", "WAL")?;
        connection.pragma_update(None, "foreign_keys", "ON")?;
        connection.execute_batch(
            "CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                event_type TEXT NOT NULL,
                path TEXT NOT NULL,
                old_path TEXT,
                timestamp TEXT NOT NULL,
                pid INTEGER NOT NULL,
                parent_pid INTEGER,
                process_path TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_events_path_timestamp
                ON events(path, timestamp);
            CREATE TABLE IF NOT EXISTS file_versions (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                path TEXT NOT NULL,
                created_at TEXT NOT NULL,
                content BLOB NOT NULL
            );
            CREATE INDEX IF NOT EXISTS idx_versions_path_created
                ON file_versions(path, created_at);",
        )?;
        Ok(Self { connection })
    }

    pub fn append(&self, event: EventRecord) -> Result<i64> {
        let parent_pid = parent_pid();
        let process_path = std::env::current_exe()
            .ok()
            .map(|path| path.to_string_lossy().into_owned());
        self.connection.execute(
            "INSERT INTO events
                (event_type, path, old_path, timestamp, pid, parent_pid, process_path)
                VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            params![
                event.kind,
                event.path.to_string_lossy(),
                event
                    .old_path
                    .map(|path| path.to_string_lossy().into_owned()),
                event.timestamp,
                event.pid,
                parent_pid,
                process_path,
            ],
        )?;
        Ok(self.connection.last_insert_rowid())
    }

    pub fn history(&self, path: Option<&Path>) -> Result<Vec<EventRecord>> {
        let mut statement = self.connection.prepare(
            "SELECT id, event_type, path, old_path, timestamp, pid
             FROM events
             WHERE (?1 IS NULL OR path = ?1)
             ORDER BY id ASC",
        )?;
        let rows = statement.query_map(
            params![path.map(|value| value.to_string_lossy().into_owned())],
            |row| {
                Ok(EventRecord {
                    id: row.get(0)?,
                    kind: row.get(1)?,
                    path: PathBuf::from(row.get::<_, String>(2)?),
                    old_path: row.get::<_, Option<String>>(3)?.map(PathBuf::from),
                    timestamp: row.get(4)?,
                    pid: row.get(5)?,
                })
            },
        )?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub fn history_under(&self, root: &Path) -> Result<Vec<EventRecord>> {
        let root = root
            .to_string_lossy()
            .trim_end_matches(['\\', '/'])
            .to_string();
        let mut statement = self.connection.prepare(
            "SELECT id, event_type, path, old_path, timestamp, pid
             FROM events
             WHERE path = ?1 OR path LIKE ?2 OR path LIKE ?3
             ORDER BY id ASC",
        )?;
        let rows = statement.query_map(
            params![root, format!("{root}\\%"), format!("{root}/%")],
            |row| {
                Ok(EventRecord {
                    id: row.get(0)?,
                    kind: row.get(1)?,
                    path: PathBuf::from(row.get::<_, String>(2)?),
                    old_path: row.get::<_, Option<String>>(3)?.map(PathBuf::from),
                    timestamp: row.get(4)?,
                    pid: row.get(5)?,
                })
            },
        )?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub fn save_version(&self, path: &Path, content: &[u8]) -> Result<i64> {
        self.connection.execute(
            "INSERT INTO file_versions (path, created_at, content) VALUES (?1, ?2, ?3)",
            params![path.to_string_lossy(), Utc::now().to_rfc3339(), content],
        )?;
        Ok(self.connection.last_insert_rowid())
    }

    pub fn versions(&self, path: &Path) -> Result<Vec<VersionRecord>> {
        let mut statement = self.connection.prepare(
            "SELECT id, path, created_at, length(content)
             FROM file_versions WHERE path = ?1 ORDER BY id DESC",
        )?;
        let rows = statement.query_map(params![path.to_string_lossy()], |row| {
            Ok(VersionRecord {
                id: row.get(0)?,
                path: PathBuf::from(row.get::<_, String>(1)?),
                created_at: row.get(2)?,
                size_bytes: row.get(3)?,
            })
        })?;
        Ok(rows.collect::<rusqlite::Result<Vec<_>>>()?)
    }

    pub fn version_content(&self, version_id: i64) -> Result<(PathBuf, Vec<u8>)> {
        Ok(self.connection.query_row(
            "SELECT path, content FROM file_versions WHERE id = ?1",
            params![version_id],
            |row| Ok((PathBuf::from(row.get::<_, String>(0)?), row.get(1)?)),
        )?)
    }
}

fn parent_pid() -> Option<u32> {
    #[cfg(unix)]
    {
        std::fs::read_to_string("/proc/self/stat")
            .ok()
            .and_then(|stat| stat.split_whitespace().nth(3)?.parse().ok())
    }
    #[cfg(windows)]
    {
        None
    }
}
