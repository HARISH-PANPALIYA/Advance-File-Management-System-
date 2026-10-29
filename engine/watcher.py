from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler
from datetime import datetime
import os
import sys
from typing import Optional

from engine.events import FileEvent
from journal import Journal


class WatcherHandler(FileSystemEventHandler):
    def __init__(self, journal: Optional[Journal] = None):
        super().__init__()
        self.journal = journal

    def _record(self, file_event):
        file_event.pid = os.getpid()
        file_event.parent_pid = os.getppid()
        file_event.process_path = sys.executable
        if self.journal is not None:
            self.journal.append(file_event)
        print_event(file_event)

    def on_created(self, event):

        if event.is_directory:
            return

        file_event = FileEvent(
            event_type="CREATE",
            path=event.src_path,
            timestamp=datetime.now()
        )

        self._record(file_event)

    def on_modified(self, event):

        if event.is_directory:
            return

        file_event = FileEvent(
            event_type="MODIFY",
            path=event.src_path,
            timestamp=datetime.now()
        )

        self._record(file_event)

    def on_deleted(self, event):

        if event.is_directory:
            return

        file_event = FileEvent(
            event_type="DELETE",
            path=event.src_path,
            timestamp=datetime.now()
        )

        self._record(file_event)

    def on_moved(self, event):

        if event.is_directory:
            return

        file_event = FileEvent(
            event_type="RENAME",
            path=event.dest_path,
            timestamp=datetime.now(),
            old_path=event.src_path
        )

        self._record(file_event)


def print_event(file_event):

    print("\n[EVENT]", file_event.event_type)

    if file_event.event_type == "RENAME":

        print("Old:", file_event.old_path)
        print("New:", file_event.path)

    else:

        print("File:", file_event.path)


def start_watcher(folder, journal_path=None):

    observer = Observer()

    journal = Journal(journal_path) if journal_path is not None else None
    handler = WatcherHandler(journal)

    observer.schedule(
        handler,
        folder,
        recursive=True
    )

    observer.start()

    print("[ENGINE] Monitoring started...")
    print("[ENGINE] Watching:", folder)

    try:

        while True:
            pass

    except KeyboardInterrupt:

        print("\n[ENGINE] Stopping...")

        observer.stop()

    observer.join()
    if journal is not None:
        journal.close()