from dataclasses import dataclass
from datetime import datetime
from typing import Optional


@dataclass
class FileEvent:
    event_type: str
    path: str
    timestamp: datetime
    old_path: Optional[str] = None
    pid: Optional[int] = None
    parent_pid: Optional[int] = None
    process_path: Optional[str] = None