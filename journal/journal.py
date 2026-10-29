"""Durable SQLite event journal for filesystem activity."""

from __future__ import annotations

import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from threading import RLock
from typing import Iterable, Optional

from engine.events import FileEvent


class Journal:
    """Persist filesystem events in a small, SQLite-backed append-only log.

    SQLite's WAL mode allows readers to inspect the journal while the watcher
    continues appending events.  The journal owns its connection and is safe
    to call from multiple watcher callback threads.
    """

    def __init__(self, database_path: str | Path) -> None:
        self.database_path = Path(database_path)
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = RLock()
        self._connection = sqlite3.connect(
            self.database_path,
            check_same_thread=False,
        )
        self._connection.row_factory = sqlite3.Row
        self._initialize()

    def _initialize(self) -> None:
        with self._lock, self._connection:
            self._connection.execute("PRAGMA journal_mode=WAL")
            self._connection.execute("PRAGMA foreign_keys=ON")
            self._connection.execute(
                """
                CREATE TABLE IF NOT EXISTS events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    event_type TEXT NOT NULL,
                    path TEXT NOT NULL,
                    old_path TEXT,
                    timestamp TEXT NOT NULL,
                    pid INTEGER,
                    parent_pid INTEGER,
                    process_path TEXT
                )
                """
            )
            self._connection.execute(
                "CREATE INDEX IF NOT EXISTS idx_events_path_timestamp "
                "ON events(path, timestamp)"
            )

    def append(self, event: FileEvent) -> int:
        """Append an event and return its journal id."""
        timestamp = event.timestamp
        if timestamp.tzinfo is None:
            timestamp = timestamp.replace(tzinfo=timezone.utc)

        with self._lock, self._connection:
            cursor = self._connection.execute(
                """
                INSERT INTO events (
                    event_type, path, old_path, timestamp,
                    pid, parent_pid, process_path
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    event.event_type,
                    event.path,
                    event.old_path,
                    timestamp.isoformat(),
                    event.pid,
                    event.parent_pid,
                    event.process_path,
                ),
            )
            return int(cursor.lastrowid)

    def events(
        self,
        *,
        path: Optional[str] = None,
        since: Optional[datetime] = None,
    ) -> list[sqlite3.Row]:
        """Return journal rows ordered from oldest to newest."""
        query = "SELECT * FROM events"
        parameters: list[str] = []
        clauses: list[str] = []
        if path is not None:
            clauses.append("path = ?")
            parameters.append(path)
        if since is not None:
            clauses.append("timestamp >= ?")
            parameters.append(since.isoformat())
        if clauses:
            query += " WHERE " + " AND ".join(clauses)
        query += " ORDER BY id ASC"

        with self._lock:
            return list(self._connection.execute(query, parameters))

    def close(self) -> None:
        with self._lock:
            self._connection.close()

    def __enter__(self) -> "Journal":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()