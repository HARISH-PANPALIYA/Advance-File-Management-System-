use crate::journal::{EventKind, EventRecord, Journal};
use anyhow::{Context, Result};
use std::fs;
use std::path::{Path, PathBuf};

pub struct BackingStore<'journal> {
    root: PathBuf,
    journal: &'journal Journal,
}

impl<'journal> BackingStore<'journal> {
    pub fn new(root: impl Into<PathBuf>, journal: &'journal Journal) -> Result<Self> {
        let root = root.into();
        fs::create_dir_all(&root)
            .with_context(|| format!("create backing directory {}", root.display()))?;
        Ok(Self { root, journal })
    }

    pub fn read(&self, relative_path: &Path) -> Result<Vec<u8>> {
        Ok(fs::read(self.resolve(relative_path))?)
    }

    pub fn write(&self, relative_path: &Path, contents: &[u8]) -> Result<()> {
        let path = self.resolve(relative_path);
        let event = if path.exists() {
            EventKind::Modify
        } else {
            EventKind::Create
        };
        self.journal.append(EventRecord::now(
            event,
            path.clone(),
            None,
            std::process::id(),
        ))?;
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(path, contents)?;
        Ok(())
    }

    pub fn delete(&self, relative_path: &Path) -> Result<()> {
        let path = self.resolve(relative_path);
        self.journal.append(EventRecord::now(
            EventKind::Delete,
            path.clone(),
            None,
            std::process::id(),
        ))?;
        fs::remove_file(path)?;
        Ok(())
    }

    pub fn rename(&self, old_path: &Path, new_path: &Path) -> Result<()> {
        let old_path = self.resolve(old_path);
        let new_path = self.resolve(new_path);
        self.journal.append(EventRecord::now(
            EventKind::Rename,
            new_path.clone(),
            Some(old_path.clone()),
            std::process::id(),
        ))?;
        if let Some(parent) = new_path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::rename(old_path, new_path)?;
        Ok(())
    }

    fn resolve(&self, relative_path: &Path) -> PathBuf {
        self.root.join(relative_path)
    }
}
