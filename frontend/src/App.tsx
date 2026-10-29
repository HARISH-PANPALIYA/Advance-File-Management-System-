import React, { useState, useEffect, useCallback, useMemo } from "react";
import {
  Folder,
  File,
  ArrowLeft,
  RotateCcw,
  RotateCw,
  RefreshCw,
  Search,
  Plus,
  FolderPlus,
  Trash2,
  Edit2,
  Copy,
  Move,
  History,
  Activity,
  Shield,
  BarChart2,
  Settings as SettingsIcon,
  CheckCircle,
  AlertTriangle,
  X,
  FileText,
  Clock,
  Archive,
  Download,
  Play,
  HardDrive,
  Copy as CopyIcon,
  Eye,
  Sliders,
  ChevronRight,
  ExternalLink,
  Code,
  Inbox,
  Layers,
  Save,
  Check,
  FileSpreadsheet,
  Sun,
  Moon,
  Edit3,
  CornerDownRight,
  Columns,
  Undo2,
  Redo2
} from "lucide-react";
import {
  api,
  FileItem,
  DirectoryListing,
  HealthStatus,
  OperationHistoryItem,
  FilesystemEvent,
  QuarantineItem,
  VersionItem,
  WatcherStatus,
  SettingsData,
  BenchmarkRecord,
  DriveInfo,
  ManagedRoot,
  ContentRevision,
  NameLocationRecord,
  FileHistoryResponse,
  ExternalCandidate,
  ExternalDiffResponse,
  FolderInsights,
  TransactionItem,
  normalizePath,
  pathBasename,
  pathDirname,
  pathJoin,
  ApiError
} from "./api";

function formatBytes(bytes: number): string {
  if (bytes === 0) return "0 B";
  const k = 1024;
  const sizes = ["B", "KB", "MB", "GB", "TB"];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return `${parseFloat((bytes / Math.pow(k, i)).toFixed(1))} ${sizes[i]}`;
}

function formatDate(ms: number | string): string {
  if (!ms) return "-";
  const d = typeof ms === "string" ? new Date(ms) : new Date(ms);
  return d.toLocaleString(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  });
}

type AppTab =
  | "files"
  | "editor"
  | "insights"
  | "external"
  | "activity"
  | "transactions"
  | "recovery"
  | "history"
  | "benchmarks"
  | "settings";

interface DiffLine {
  type: "added" | "deleted" | "unchanged";
  oldLineNum?: number;
  newLineNum?: number;
  text: string;
}

function computeLineDiff(oldText: string, newText: string): DiffLine[] {
  const oldLines = oldText ? oldText.split(/\r?\n/) : [];
  const newLines = newText ? newText.split(/\r?\n/) : [];

  if (oldLines.length === 0 && newLines.length === 0) return [];
  if (oldLines.length === 0) {
    return newLines.map((line, idx) => ({
      type: "added",
      newLineNum: idx + 1,
      text: line
    }));
  }
  if (newLines.length === 0) {
    return oldLines.map((line, idx) => ({
      type: "deleted",
      oldLineNum: idx + 1,
      text: line
    }));
  }

  const diff: DiffLine[] = [];
  let i = 0, j = 0;
  while (i < oldLines.length && j < newLines.length) {
    if (oldLines[i] === newLines[j]) {
      diff.push({ type: "unchanged", oldLineNum: i + 1, newLineNum: j + 1, text: oldLines[i] });
      i++; j++;
    } else {
      let foundOldInNew = -1;
      for (let k = j + 1; k < Math.min(j + 6, newLines.length); k++) {
        if (newLines[k] === oldLines[i]) {
          foundOldInNew = k;
          break;
        }
      }
      if (foundOldInNew !== -1) {
        while (j < foundOldInNew) {
          diff.push({ type: "added", newLineNum: j + 1, text: newLines[j] });
          j++;
        }
      } else {
        diff.push({ type: "deleted", oldLineNum: i + 1, text: oldLines[i] });
        i++;
      }
    }
  }
  while (i < oldLines.length) {
    diff.push({ type: "deleted", oldLineNum: i + 1, text: oldLines[i] });
    i++;
  }
  while (j < newLines.length) {
    diff.push({ type: "added", newLineNum: j + 1, text: newLines[j] });
    j++;
  }
  return diff;
}

export function App() {
  const [activeTab, setActiveTab] = useState<AppTab>("files");

  // Health and undo/redo global state
  const [health, setHealth] = useState<HealthStatus | null>(null);
  const [serverOnline, setServerOnline] = useState<boolean>(true);
  const [toast, setToast] = useState<{ message: string; type: "info" | "warning" | "error" } | null>(null);

  // System drives & managed roots
  const [systemDrives, setSystemDrives] = useState<DriveInfo[]>([]);
  const [managedRoots, setManagedRoots] = useState<ManagedRoot[]>([]);

  // File browser state
  const [currentPath, setCurrentPath] = useState<string>("");
  const [listing, setListing] = useState<DirectoryListing | null>(null);
  const [loadingFiles, setLoadingFiles] = useState<boolean>(false);
  const [searchQuery, setSearchQuery] = useState<string>("");
  const [selectedPaths, setSelectedPaths] = useState<Set<string>>(new Set());
  const [fileFilter, setFileFilter] = useState<"all" | "dirs" | "docs" | "code">("all");
  const [sortField, setSortField] = useState<"name" | "size" | "modified">("name");
  const [sortAsc, setSortAsc] = useState<boolean>(true);

  // Inspector Panel state
  const [inspectedItem, setInspectedItem] = useState<FileItem | null>(null);
  const [inspectedContent, setInspectedContent] = useState<string | null>(null);
  const [loadingContent, setLoadingContent] = useState<boolean>(false);

  // In-App Document Editor state
  const [editorPath, setEditorPath] = useState<string>("");
  const [editorContent, setEditorContent] = useState<string>("");
  const [editorPreHash, setEditorPreHash] = useState<string>("");
  const [editorDirty, setEditorDirty] = useState<boolean>(false);
  const [editorSaving, setEditorSaving] = useState<boolean>(false);
  const [editorStatus, setEditorStatus] = useState<"saved" | "unsaved" | "conflict">("saved");
  const [conflictModal, setConflictModal] = useState<{ open: boolean; currentHash: string; serverError: string } | null>(null);

  // Folder Insights state
  const [insightsPath, setInsightsPath] = useState<string>("");
  const [folderInsights, setFolderInsights] = useState<FolderInsights | null>(null);
  const [loadingInsights, setLoadingInsights] = useState<boolean>(false);

  // External Changes Inbox state
  const [externalCandidates, setExternalCandidates] = useState<ExternalCandidate[]>([]);
  const [externalStatusFilter, setExternalStatusFilter] = useState<string>("PENDING");
  const [selectedCandidateDiff, setSelectedCandidateDiff] = useState<ExternalDiffResponse | null>(null);
  const [diffModalOpen, setDiffModalOpen] = useState<boolean>(false);

  // Activity events state
  const [watcherStatus, setWatcherStatus] = useState<WatcherStatus | null>(null);
  const [events, setEvents] = useState<FilesystemEvent[]>([]);
  const [eventFilter, setEventFilter] = useState<"all" | "external" | "app">("all");

  // Batch Transactions state
  const [transactions, setTransactions] = useState<TransactionItem[]>([]);

  // Quarantine Vault state
  const [quarantineItems, setQuarantineItems] = useState<QuarantineItem[]>([]);

  // File History state
  const [historyTarget, setHistoryTarget] = useState<string>("");
  const [fileHistory, setFileHistory] = useState<FileHistoryResponse | null>(null);
  const [activeHistorySubTab, setActiveHistorySubTab] = useState<"content_revisions" | "name_location" | "external_changes" | "operations">("content_revisions");

  // Benchmarks state
  const [benchmarks, setBenchmarks] = useState<BenchmarkRecord[]>([]);
  const [runningBench, setRunningBench] = useState<string | null>(null);
  const [benchWatcherResult, setBenchWatcherResult] = useState<any | null>(null);
  const [benchScanResult, setBenchScanResult] = useState<any | null>(null);
  const [benchJournalResult, setBenchJournalResult] = useState<any | null>(null);

  // Settings state
  const [settings, setSettings] = useState<SettingsData | null>(null);

  // Modal dialogs state
  const [createModal, setCreateModal] = useState<"file" | "dir" | null>(null);
  const [newEntityName, setNewEntityName] = useState<string>("");
  const [newRootPath, setNewRootPath] = useState<string>("");
  const [showAddRootModal, setShowAddRootModal] = useState<boolean>(false);

  // Theme state
  const [theme, setTheme] = useState<"light" | "dark">(() => {
    const saved = localStorage.getItem("fileguard-theme");
    if (saved === "dark" || saved === "light") return saved;
    return "light";
  });

  useEffect(() => {
    document.documentElement.setAttribute("data-theme", theme);
    localStorage.setItem("fileguard-theme", theme);
  }, [theme]);

  const toggleTheme = () => {
    setTheme((prev) => (prev === "light" ? "dark" : "light"));
  };

  // Rename modal state
  const [renameTarget, setRenameTarget] = useState<FileItem | null>(null);
  const [newRenameName, setNewRenameName] = useState<string>("");
  const renameInputRef = React.useRef<HTMLInputElement | null>(null);

  // Quarantine & Batch Action Modals state
  const [quarantineModal, setQuarantineModal] = useState<{
    open: boolean;
    items: FileItem[];
    reason: string;
  } | null>(null);

  const [batchMoveModal, setBatchMoveModal] = useState<{
    open: boolean;
    targetDir: string;
  } | null>(null);

  const [restoreCustomModal, setRestoreCustomModal] = useState<{
    open: boolean;
    item: QuarantineItem;
    customDest: string;
  } | null>(null);

  const [restoreConflictModal, setRestoreConflictModal] = useState<{
    open: boolean;
    item: QuarantineItem;
    destination: string;
    errorMsg: string;
  } | null>(null);

  const [quarantineDetailModal, setQuarantineDetailModal] = useState<QuarantineItem | null>(null);
  const [purgeConfirmModal, setPurgeConfirmModal] = useState<QuarantineItem | null>(null);
  const [expandedTxId, setExpandedTxId] = useState<string | null>(null);

  // Visual Diff state
  const [diffMode, setDiffMode] = useState<"unified" | "split">("unified");
  const [selectedCandidateItem, setSelectedCandidateItem] = useState<ExternalCandidate | null>(null);

  const openRenameModal = (item: FileItem) => {
    setRenameTarget(item);
    setNewRenameName(item.name);
  };

  const openQuarantineModal = (items: FileItem[]) => {
    if (!items || items.length === 0) return;
    setQuarantineModal({
      open: true,
      items,
      reason: "User initiated quarantine"
    });
  };

  useEffect(() => {
    if (renameTarget && renameInputRef.current) {
      renameInputRef.current.focus();
      const dotIdx = renameTarget.name.lastIndexOf(".");
      if (dotIdx > 0 && !renameTarget.is_directory) {
        renameInputRef.current.setSelectionRange(0, dotIdx);
      } else {
        renameInputRef.current.select();
      }
    }
  }, [renameTarget]);

  // F2 keydown listener for renaming active/inspected item
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === "F2" && activeTab === "files" && inspectedItem) {
        e.preventDefault();
        openRenameModal(inspectedItem);
      }
    };
    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [activeTab, inspectedItem]);

  const handleRenameSubmit = async () => {
    if (!renameTarget || !newRenameName.trim() || newRenameName.trim() === renameTarget.name) {
      setRenameTarget(null);
      return;
    }
    const parent = pathDirname(renameTarget.path);
    const newPath = pathJoin(parent, newRenameName.trim());
    try {
      const res = await api.renameItem(renameTarget.path, newPath);
      if (res.success) {
        showToast(`Renamed ${renameTarget.name} to ${newRenameName.trim()}`, "info");
        setRenameTarget(null);
        loadDirectory(currentPath);
        checkHealth();
        if (inspectedItem?.path === renameTarget.path) {
          setInspectedItem({
            ...renameTarget,
            name: newRenameName.trim(),
            path: newPath
          });
        }
      } else {
        showToast(res.error || "Rename failed", "error");
      }
    } catch (err: any) {
      showToast(err.message || "Rename failed", "error");
    }
  };

  // Quarantine & Batch Action Handlers
  const handleQuarantineConfirm = async () => {
    if (!quarantineModal || quarantineModal.items.length === 0) return;
    try {
      if (quarantineModal.items.length === 1) {
        const item = quarantineModal.items[0];
        const res = await api.quarantineItem(item.path, quarantineModal.reason);
        if (res.success) {
          showToast(`Quarantined ${item.name} into secure vault`, "info");
        } else {
          showToast(res.error || "Quarantine failed", "error");
        }
      } else {
        const paths = quarantineModal.items.map((i) => i.path);
        const res = await api.batchDelete(paths);
        if (res.success) {
          showToast(`Quarantined ${paths.length} items to vault`, "info");
        } else {
          showToast("Batch quarantine failed", "error");
        }
      }
      setQuarantineModal(null);
      setSelectedPaths(new Set());
      checkHealth();
      loadDirectory(currentPath);
      api.getQuarantine().then(setQuarantineItems).catch(() => {});
    } catch (err: any) {
      showToast(err.message || "Failed to quarantine item", "error");
    }
  };

  const handleBatchMoveConfirm = async () => {
    if (!batchMoveModal || !batchMoveModal.targetDir.trim()) return;
    const target = batchMoveModal.targetDir.trim();
    const selectedItems = (listing?.items || []).filter((i) => selectedPaths.has(i.path));
    if (selectedItems.length === 0) return;

    const items = selectedItems.map((i) => ({
      source: i.path,
      destination: pathJoin(target, i.name)
    }));

    try {
      const res = await api.batchMove(items);
      if (res.success) {
        showToast(`Batch moved ${items.length} item(s) to ${target}`, "info");
        setBatchMoveModal(null);
        setSelectedPaths(new Set());
        checkHealth();
        loadDirectory(currentPath);
        api.getTransactions().then(setTransactions).catch(() => {});
      } else {
        showToast("Batch move failed", "error");
      }
    } catch (err: any) {
      showToast(err.message || "Batch move failed", "error");
    }
  };

  const handleRestoreQuarantine = async (q: QuarantineItem, customDest?: string, overwrite = false) => {
    try {
      const res = await api.restoreQuarantine(q.id, customDest, overwrite);
      if (res.success) {
        showToast(res.message || "File restored from quarantine", "info");
        setRestoreCustomModal(null);
        setRestoreConflictModal(null);
        api.getQuarantine().then(setQuarantineItems);
        loadDirectory(currentPath);
        checkHealth();
      }
    } catch (err: any) {
      const msg = err.message || "Failed to restore";
      if (msg.includes("conflict") || msg.includes("already exists")) {
        setRestoreConflictModal({
          open: true,
          item: q,
          destination: customDest || q.original_path,
          errorMsg: msg
        });
      } else {
        showToast(msg, "error");
      }
    }
  };

  const handlePurgeQuarantine = async (q: QuarantineItem) => {
    try {
      const res = await api.deleteQuarantine(q.id);
      if (res.success) {
        showToast("Item permanently purged from quarantine vault", "info");
        setPurgeConfirmModal(null);
        api.getQuarantine().then(setQuarantineItems);
      }
    } catch (err: any) {
      showToast(err.message || "Failed to purge item", "error");
    }
  };

  const handleUndoTransaction = async (txId: string) => {
    try {
      const res = await api.undoTransaction(txId);
      if (res.success) {
        showToast(res.message || "Batch transaction rolled back successfully", "info");
        api.getTransactions().then(setTransactions);
        checkHealth();
        loadDirectory(currentPath);
      } else {
        showToast(res.error || "Failed to rollback transaction", "error");
      }
    } catch (err: any) {
      showToast(err.message || "Failed to rollback transaction", "error");
    }
  };

  const handleRedoTransaction = async (txId: string) => {
    try {
      const res = await api.redoTransaction(txId);
      if (res.success) {
        showToast(res.message || "Batch transaction reapplied successfully", "info");
        api.getTransactions().then(setTransactions);
        checkHealth();
        loadDirectory(currentPath);
      } else {
        showToast(res.error || "Failed to reapply transaction", "error");
      }
    } catch (err: any) {
      showToast(err.message || "Failed to reapply transaction", "error");
    }
  };

  // Show Toast
  const showToast = useCallback((message: string, type: "info" | "warning" | "error" = "info") => {
    setToast({ message, type });
    setTimeout(() => {
      setToast((prev) => (prev?.message === message ? null : prev));
    }, 4500);
  }, []);

  // Poll Health
  const checkHealth = useCallback(async () => {
    try {
      const h = await api.getHealth();
      setHealth(h);
      setServerOnline(true);
    } catch {
      setServerOnline(false);
    }
  }, []);

  // Load System Drives & Roots
  const refreshRootsAndDrives = useCallback(async () => {
    try {
      const [drives, roots] = await Promise.all([
        api.getSystemDrives().catch(() => []),
        api.getRoots().catch(() => [])
      ]);
      setSystemDrives(drives);
      setManagedRoots(roots);
    } catch (e) {
      console.error("Failed to load drives or roots", e);
    }
  }, []);

  // Load Directory Files
  const loadDirectory = useCallback(async (path?: string) => {
    setLoadingFiles(true);
    try {
      const data = await api.getFiles(path);
      setListing(data);
      setCurrentPath(data.current_path);
      setSelectedPaths(new Set());
      setInspectedItem(null);
      setInspectedContent(null);
    } catch (err: any) {
      showToast(err.message || "Failed to load directory", "error");
    } finally {
      setLoadingFiles(false);
    }
  }, [showToast]);

  // Load External Changes
  const loadExternalChanges = useCallback(async () => {
    try {
      const list = await api.getExternalChanges(externalStatusFilter === "ALL" ? undefined : externalStatusFilter);
      setExternalCandidates(list);
    } catch (err: any) {
      console.error(err);
    }
  }, [externalStatusFilter]);

  // Load Insights
  const loadInsights = useCallback(async (path?: string) => {
    setLoadingInsights(true);
    try {
      const data = await api.getFolderInsights(path || currentPath);
      setFolderInsights(data);
      setInsightsPath(data.folder_path);
    } catch (err: any) {
      showToast(err.message || "Failed to generate insights", "error");
    } finally {
      setLoadingInsights(false);
    }
  }, [currentPath, showToast]);

  // Initial Load & SSE Setup
  useEffect(() => {
    checkHealth();
    refreshRootsAndDrives();
    loadDirectory();

    const interval = setInterval(checkHealth, 3000);

    // SSE Activity events listener
    const eventSource = new EventSource(api.getEventStreamUrl());
    eventSource.onmessage = (event) => {
      try {
        const ev: FilesystemEvent = JSON.parse(event.data);
        setEvents((prev) => [ev, ...prev.slice(0, 199)]);
        if (ev.is_external) {
          loadExternalChanges();
        }
      } catch {}
    };

    return () => {
      clearInterval(interval);
      eventSource.close();
    };
  }, [checkHealth, refreshRootsAndDrives, loadDirectory, loadExternalChanges]);

  // Watcher Status
  const refreshWatcherStatus = useCallback(async () => {
    try {
      const st = await api.getWatchStatus();
      setWatcherStatus(st);
    } catch {}
  }, []);

  useEffect(() => {
    refreshWatcherStatus();
  }, [refreshWatcherStatus]);

  // Load inspected item content
  useEffect(() => {
    if (!inspectedItem || inspectedItem.is_directory) {
      setInspectedContent(null);
      return;
    }
    let cancelled = false;
    setLoadingContent(true);
    api
      .getFileContent(inspectedItem.path)
      .then((res) => {
        if (!cancelled) setInspectedContent(res.content);
      })
      .catch(() => {
        if (!cancelled) setInspectedContent("(Binary or unreadable file preview)");
      })
      .finally(() => {
        if (!cancelled) setLoadingContent(false);
      });

    return () => {
      cancelled = true;
    };
  }, [inspectedItem]);

  // Global Keyboard Shortcuts (Ctrl+Z: Content Undo, Ctrl+Y: Content Redo, Alt+Z: Name Undo, Alt+Y: Name Redo)
  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.altKey && e.key.toLowerCase() === "z") {
        if (e.shiftKey) {
          if (health?.can_redo_name) handleRedo("name");
        } else {
          if (health?.can_undo_name) handleUndo("name");
        }
        e.preventDefault();
      } else if (e.altKey && e.key.toLowerCase() === "y") {
        if (health?.can_redo_name) handleRedo("name");
        e.preventDefault();
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "z") {
        if (e.shiftKey) {
          if (health?.can_redo_content) handleRedo("content");
          else if (health?.can_redo) handleRedo("all");
        } else {
          if (health?.can_undo_content) handleUndo("content");
          else if (health?.can_undo) handleUndo("all");
        }
        e.preventDefault();
      } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "y") {
        if (health?.can_redo_content) handleRedo("content");
        else if (health?.can_redo) handleRedo("all");
        e.preventDefault();
      }
    };
    window.addEventListener("keydown", handleKeyDown);
    return () => window.removeEventListener("keydown", handleKeyDown);
  }, [health]);

  // Undo / Redo Handlers with Independent Scope ("content" | "name" | "all")
  const handleUndo = async (scope: "content" | "name" | "all" = "all") => {
    try {
      const res = await api.undo(scope);
      if (res.success) {
        showToast(res.message || "Undo executed successfully", "info");
        checkHealth();
        loadDirectory(currentPath);
        api.getTransactions().then(setTransactions).catch(() => {});
        if (historyTarget) openFileHistory(historyTarget);
      } else if (res.has_conflict) {
        showToast(`Undo conflict: ${res.conflict_reason || "File was altered"}`, "error");
      } else {
        showToast(res.error || "Undo failed", "error");
      }
    } catch (err: any) {
      showToast(err.message || "Undo failed", "error");
    }
  };

  const handleRedo = async (scope: "content" | "name" | "all" = "all") => {
    try {
      const res = await api.redo(scope);
      if (res.success) {
        showToast(res.message || "Redo executed successfully", "info");
        checkHealth();
        loadDirectory(currentPath);
        api.getTransactions().then(setTransactions).catch(() => {});
        if (historyTarget) openFileHistory(historyTarget);
      } else if (res.has_conflict) {
        showToast(`Redo conflict: ${res.conflict_reason || "Target conflict"}`, "error");
      } else {
        showToast(res.error || "Redo failed", "error");
      }
    } catch (err: any) {
      showToast(err.message || "Redo failed", "error");
    }
  };

  const openDiffForCandidate = async (cand: ExternalCandidate) => {
    try {
      const diff = await api.getExternalChangeDiff(cand.id);
      setSelectedCandidateDiff(diff);
      setSelectedCandidateItem(cand);
      setDiffModalOpen(true);
    } catch (e: any) {
      showToast(e.message || "Failed to load diff", "error");
    }
  };

  // Open file in In-App Document Editor
  const openInEditor = async (path: string) => {
    try {
      const res = await api.getFileContent(path);
      setEditorPath(path);
      setEditorContent(res.content);
      setEditorPreHash(inspectedItem?.hash || "");
      setEditorDirty(false);
      setEditorStatus("saved");
      setActiveTab("editor");
    } catch (err: any) {
      showToast("Cannot open file in editor: " + err.message, "error");
    }
  };

  // Save Document
  const handleSaveDocument = async (forceOverwrite = false) => {
    if (!editorPath) return;
    setEditorSaving(true);
    try {
      const res = await api.saveDocument(
        editorPath,
        editorContent,
        forceOverwrite ? "" : editorPreHash,
        "Edited in FileGuard Document Editor"
      );
      if (res.success) {
        setEditorPreHash(res.new_hash || "");
        setEditorDirty(false);
        setEditorStatus("saved");
        showToast(`Saved ${pathBasename(editorPath)} (Rev ${res.revision_number})`, "info");
        checkHealth();
        if (conflictModal) setConflictModal(null);
      }
    } catch (err: any) {
      if (err.status === 409 || err.data?.conflict) {
        setEditorStatus("conflict");
        setConflictModal({
          open: true,
          currentHash: err.data?.current_hash || "",
          serverError: err.message
        });
      } else {
        showToast(err.message || "Failed to save file", "error");
      }
    } finally {
      setEditorSaving(false);
    }
  };

  // Open File History
  const openFileHistory = async (path: string) => {
    setHistoryTarget(path);
    setActiveTab("history");
    try {
      const h = await api.getFileHistory(path);
      setFileHistory(h);
    } catch (err: any) {
      showToast(err.message || "Failed to load file history", "error");
    }
  };

  // Filtered & Sorted File Items
  const filteredItems = useMemo(() => {
    if (!listing) return [];
    let items = [...listing.items];

    // Filter by type
    if (fileFilter === "dirs") items = items.filter((i) => i.is_directory);
    else if (fileFilter === "docs") items = items.filter((i) => ["txt", "md", "json", "csv", "log"].includes(i.extension));
    else if (fileFilter === "code") items = items.filter((i) => ["cpp", "c", "h", "hpp", "py", "ts", "js", "html"].includes(i.extension));

    // Filter by search
    if (searchQuery.trim()) {
      const q = searchQuery.toLowerCase();
      items = items.filter((i) => i.name.toLowerCase().includes(q));
    }

    // Sort
    items.sort((a, b) => {
      if (a.is_directory !== b.is_directory) return a.is_directory ? -1 : 1;
      let cmp = 0;
      if (sortField === "name") cmp = a.name.localeCompare(b.name);
      else if (sortField === "size") cmp = a.size_bytes - b.size_bytes;
      else if (sortField === "modified") cmp = a.modified_at - b.modified_at;
      return sortAsc ? cmp : -cmp;
    });

    return items;
  }, [listing, fileFilter, searchQuery, sortField, sortAsc]);

  // Bulk Delete
  const handleBulkDelete = async () => {
    if (selectedPaths.size === 0) return;
    if (!confirm(`Are you sure you want to move ${selectedPaths.size} item(s) to quarantine? You can undo this action.`)) return;

    try {
      const paths = Array.from(selectedPaths);
      const res = await api.batchDelete(paths);
      if (res.success) {
        showToast(`Moved ${paths.length} items to quarantine`, "info");
        checkHealth();
        loadDirectory(currentPath);
      }
    } catch (err: any) {
      showToast(err.message || "Batch delete failed", "error");
    }
  };

  // Populate Demo Workspace
  const handlePopulateDemo = async () => {
    try {
      const res = await api.populateDemoWorkspace();
      if (res.success) {
        showToast("Demo workspace populated at " + res.path, "info");
        refreshRootsAndDrives();
        loadDirectory(res.path);
      }
    } catch (err: any) {
      showToast("Demo population failed: " + err.message, "error");
    }
  };

  // Add Managed Root
  const handleAddRoot = async () => {
    if (!newRootPath.trim()) return;
    try {
      const res = await api.addRoot(newRootPath.trim());
      if (res.success) {
        showToast("Added managed root", "info");
        setShowAddRootModal(false);
        setNewRootPath("");
        refreshRootsAndDrives();
      }
    } catch (err: any) {
      showToast(err.message || "Failed to add root", "error");
    }
  };

  // Create Entity
  const handleCreateEntity = async () => {
    if (!newEntityName.trim()) return;
    const target = pathJoin(currentPath, newEntityName.trim());
    try {
      if (createModal === "file") {
        await api.createFile(target);
        showToast(`Created file ${newEntityName}`, "info");
      } else {
        await api.createDirectory(target);
        showToast(`Created folder ${newEntityName}`, "info");
      }
      setCreateModal(null);
      setNewEntityName("");
      checkHealth();
      loadDirectory(currentPath);
    } catch (err: any) {
      showToast(err.message || "Creation failed", "error");
    }
  };

  return (
    <div className="app-shell">
      {/* ── Toast Notification ─────────────────────── */}
      {toast && (
        <div className={`toast-notice ${toast.type}`}>
          {toast.type === "error" ? <AlertTriangle size={16} /> : <CheckCircle size={16} />}
          <span>{toast.message}</span>
          <button className="btn-icon" onClick={() => setToast(null)} style={{ background: "transparent", border: "none", color: "#fff" }}>
            <X size={14} />
          </button>
        </div>
      )}

      {/* ── Sidebar ────────────────────────────────── */}
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-icon">
            <Shield size={18} />
          </div>
          <div>
            <div className="brand-title">FileGuard</div>
            <div className="brand-subtitle">OS Storage & Recovery</div>
          </div>
        </div>

        <div className="sidebar-scroll">
          {/* System Drives */}
          <div>
            <div className="nav-section-title">My Computer (Drives)</div>
            <div className="drives-list">
              {systemDrives.length > 0 ? (
                systemDrives.map((d) => (
                  <div
                    key={d.letter}
                    className="drive-item"
                    onClick={() => {
                      loadDirectory(d.path);
                      setActiveTab("files");
                    }}
                  >
                    <HardDrive size={15} color="#1b4d3e" />
                    <div className="drive-meta">
                      <div className="drive-label">{d.label}</div>
                      <div className="drive-free">{formatBytes(d.free_bytes)} free of {formatBytes(d.total_bytes)}</div>
                    </div>
                  </div>
                ))
              ) : (
                <div style={{ fontSize: 11, color: "var(--text-muted)", padding: "4px 8px" }}>Scanning drives...</div>
              )}
            </div>
          </div>

          {/* Managed Roots & Workspaces */}
          <div>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
              <div className="nav-section-title">Managed Roots</div>
              <button
                className="btn btn-sm btn-icon"
                title="Add Root Folder"
                onClick={() => setShowAddRootModal(true)}
                style={{ padding: 2, height: 18 }}
              >
                <Plus size={11} />
              </button>
            </div>
            <div className="drives-list">
              {managedRoots.map((r) => (
                <div
                  key={r.path}
                  className="drive-item"
                  onClick={() => {
                    loadDirectory(r.path);
                    setActiveTab("files");
                  }}
                >
                  <Folder size={14} color="#b45309" />
                  <div className="drive-meta">
                    <div className="drive-label">{r.name}</div>
                    <div className="drive-free" style={{ fontFamily: "var(--font-mono)" }}>{pathBasename(r.path)}</div>
                  </div>
                </div>
              ))}
              <button
                className="btn btn-sm"
                style={{ marginTop: 4, width: "100%", justifyContent: "center", fontSize: 11 }}
                onClick={handlePopulateDemo}
              >
                Reset Demo Workspace (watched/)
              </button>
            </div>
          </div>

          {/* Navigation Views */}
          <div>
            <div className="nav-section-title">Workspaces & Lab</div>
            <nav style={{ display: "flex", flexDirection: "column", gap: 2 }}>
              <div
                className={`nav-item ${activeTab === "files" ? "active" : ""}`}
                onClick={() => setActiveTab("files")}
              >
                <Folder size={15} />
                <span>File Explorer</span>
              </div>

              <div
                className={`nav-item ${activeTab === "editor" ? "active" : ""}`}
                onClick={() => setActiveTab("editor")}
              >
                <FileText size={15} />
                <span>Document Editor</span>
                {editorDirty && <span className="nav-badge warning">Unsaved</span>}
              </div>

              <div
                className={`nav-item ${activeTab === "insights" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("insights");
                  loadInsights();
                }}
              >
                <BarChart2 size={15} />
                <span>Folder Insights</span>
              </div>

              <div
                className={`nav-item ${activeTab === "external" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("external");
                  loadExternalChanges();
                }}
              >
                <Inbox size={15} />
                <span>External Changes</span>
                {externalCandidates.filter((c) => c.status === "PENDING").length > 0 && (
                  <span className="nav-badge danger">
                    {externalCandidates.filter((c) => c.status === "PENDING").length}
                  </span>
                )}
              </div>

              <div
                className={`nav-item ${activeTab === "activity" ? "active" : ""}`}
                onClick={() => setActiveTab("activity")}
              >
                <Activity size={15} />
                <span>Activity Stream</span>
              </div>

              <div
                className={`nav-item ${activeTab === "transactions" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("transactions");
                  api.getTransactions().then(setTransactions);
                }}
              >
                <Layers size={15} />
                <span>Batch Transactions</span>
              </div>

              <div
                className={`nav-item ${activeTab === "recovery" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("recovery");
                  api.getQuarantine().then(setQuarantineItems);
                }}
              >
                <Archive size={15} />
                <span>Quarantine Vault</span>
                {quarantineItems.length > 0 && <span className="nav-badge">{quarantineItems.length}</span>}
              </div>

              <div
                className={`nav-item ${activeTab === "history" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("history");
                  if (currentPath) openFileHistory(currentPath);
                }}
              >
                <History size={15} />
                <span>File History</span>
              </div>

              <div
                className={`nav-item ${activeTab === "benchmarks" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("benchmarks");
                  api.getBenchmarks().then(setBenchmarks);
                }}
              >
                <Sliders size={15} />
                <span>OS Performance Lab</span>
              </div>

              <div
                className={`nav-item ${activeTab === "settings" ? "active" : ""}`}
                onClick={() => {
                  setActiveTab("settings");
                  api.getSettings().then(setSettings);
                }}
              >
                <SettingsIcon size={15} />
                <span>Settings & Storage</span>
              </div>
            </nav>
          </div>
        </div>

        <div className="sidebar-footer">
          <div>
            <span className={`status-dot ${serverOnline ? "" : "offline"}`} />
            <span>{serverOnline ? "C++ Core Active" : "C++ Disconnected"}</span>
          </div>
          <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--text-muted)" }}>v2.0 WAL</span>
        </div>
      </aside>

      {/* ── Main Work Area ─────────────────────────── */}
      <div className="main-wrapper">
        {/* Top Header & Global Actions */}
        <header className="top-header">
          {/* Breadcrumbs */}
          <div className="breadcrumb-bar">
            {listing?.parent_path && (
              <button
                className="btn btn-sm btn-icon"
                title="Go to Parent Folder"
                onClick={() => loadDirectory(listing.parent_path)}
              >
                <ArrowLeft size={13} />
              </button>
            )}
            <span style={{ color: "var(--text-muted)" }}>/</span>
            {currentPath ? (
              normalizePath(currentPath)
                .split("/")
                .filter(Boolean)
                .map((seg, idx, arr) => {
                  const subPath = arr.slice(0, idx + 1).join("/");
                  const isLast = idx === arr.length - 1;
                  return (
                    <React.Fragment key={subPath}>
                      <span
                        className={`breadcrumb-crumb ${isLast ? "active" : ""}`}
                        onClick={() => loadDirectory(subPath)}
                      >
                        {seg}
                      </span>
                      {!isLast && <span style={{ color: "var(--text-muted)" }}>/</span>}
                    </React.Fragment>
                  );
                })
            ) : (
              <span className="breadcrumb-crumb active">Root</span>
            )}
          </div>

          {/* Quick Actions Toolbar */}
          <div className="header-actions">
            {/* Content Undo / Redo */}
            <div style={{ display: "inline-flex", gap: 3 }}>
              <button
                className="btn btn-sm"
                title={
                  health?.can_undo_content
                    ? `Undo content edit (Ctrl+Z): ${health.last_undo_content_op?.source || health.last_undo_content_op?.dest || ""}`
                    : "No content edits to undo"
                }
                disabled={!health?.can_undo_content}
                onClick={() => handleUndo("content")}
              >
                <RotateCcw size={12} />
                <span>Content Undo</span>
              </button>

              <button
                className="btn btn-sm"
                title={
                  health?.can_redo_content
                    ? `Redo content edit (Ctrl+Y): ${health.last_redo_content_op?.source || health.last_redo_content_op?.dest || ""}`
                    : "No content edits to redo"
                }
                disabled={!health?.can_redo_content}
                onClick={() => handleRedo("content")}
              >
                <RotateCw size={12} />
                <span>Content Redo</span>
              </button>
            </div>

            {/* Name / Location Undo / Redo */}
            <div style={{ display: "inline-flex", gap: 3 }}>
              <button
                className="btn btn-sm"
                title={
                  health?.can_undo_name
                    ? (health.last_undo_name_op?.transaction_id
                        ? `Undo batch operation (${health.last_undo_name_op.transaction_id}) (Alt+Z)`
                        : `Undo rename or move (Alt+Z): ${health.last_undo_name_op?.dest || ""}`)
                    : "No rename/move to undo"
                }
                disabled={!health?.can_undo_name}
                onClick={() => handleUndo("name")}
              >
                <Undo2 size={12} />
                <span>{health?.last_undo_name_op?.transaction_id ? "Batch Undo" : "Name Undo"}</span>
              </button>

              <button
                className="btn btn-sm"
                title={
                  health?.can_redo_name
                    ? (health.last_redo_name_op?.transaction_id
                        ? `Redo batch operation (${health.last_redo_name_op.transaction_id}) (Alt+Y)`
                        : `Redo rename or move (Alt+Y): ${health.last_redo_name_op?.dest || ""}`)
                    : "No rename/move to redo"
                }
                disabled={!health?.can_redo_name}
                onClick={() => handleRedo("name")}
              >
                <Redo2 size={12} />
                <span>{health?.last_redo_name_op?.transaction_id ? "Batch Redo" : "Name Redo"}</span>
              </button>
            </div>

            {/* General / Batch / Delete Undo if applicable */}
            {health?.can_undo && !health?.can_undo_content && !health?.can_undo_name && (
              <div style={{ display: "inline-flex", gap: 3 }}>
                <button
                  className="btn btn-sm btn-warning"
                  title={`Undo ${health.last_undo_op?.type || "action"}${health.last_undo_op?.transaction_id ? ` (${health.last_undo_op.transaction_id})` : ""}: ${health.last_undo_op?.dest || health.last_undo_op?.source || ""}`}
                  onClick={() => handleUndo("all")}
                >
                  <RotateCcw size={12} />
                  <span>Undo {health.last_undo_op?.transaction_id ? "Batch" : (health.last_undo_op?.type === "DELETE" ? "Delete" : "Action")}</span>
                </button>
              </div>
            )}

            <button
              className="btn btn-sm"
              title="Refresh Directory"
              onClick={() => loadDirectory(currentPath)}
            >
              <RefreshCw size={12} />
            </button>

            <button
              className="btn btn-sm"
              title="New File in Current Folder"
              onClick={() => setCreateModal("file")}
            >
              <Plus size={12} />
              <span>File</span>
            </button>

            <button
              className="btn btn-sm"
              title="New Folder in Current Folder"
              onClick={() => setCreateModal("dir")}
            >
              <FolderPlus size={12} />
              <span>Folder</span>
            </button>

            {selectedPaths.size > 0 && (
              <button
                className="btn btn-sm btn-danger"
                onClick={handleBulkDelete}
              >
                <Trash2 size={13} />
                <span>Delete ({selectedPaths.size})</span>
              </button>
            )}

            <button
              className="theme-toggle-btn"
              title={`Switch to ${theme === "light" ? "Dark" : "Light"} Theme`}
              onClick={toggleTheme}
            >
              {theme === "light" ? <Moon size={13} /> : <Sun size={13} color="#fbbf24" />}
              <span>{theme === "light" ? "Dark" : "Light"}</span>
            </button>
          </div>
        </header>

        {/* Dynamic Content Views */}
        <div className="content-body">
          {/* ──────────────── 1. FILE EXPLORER VIEW ──────────────── */}
          {activeTab === "files" && (
            <div className="file-browser-view">
              <div className="browser-toolbar">
                <div className="search-input-box">
                  <Search size={14} color="var(--text-muted)" />
                  <input
                    type="text"
                    placeholder="Search files or filter..."
                    value={searchQuery}
                    onChange={(e) => setSearchQuery(e.target.value)}
                  />
                  {searchQuery && (
                    <button
                      className="btn-icon"
                      onClick={() => setSearchQuery("")}
                      style={{ background: "transparent", border: "none" }}
                    >
                      <X size={12} />
                    </button>
                  )}
                </div>

                <div className="filter-pills">
                  <span
                    className={`pill ${fileFilter === "all" ? "active" : ""}`}
                    onClick={() => setFileFilter("all")}
                  >
                    All ({listing?.items.length || 0})
                  </span>
                  <span
                    className={`pill ${fileFilter === "dirs" ? "active" : ""}`}
                    onClick={() => setFileFilter("dirs")}
                  >
                    Folders ({listing?.total_directories || 0})
                  </span>
                  <span
                    className={`pill ${fileFilter === "docs" ? "active" : ""}`}
                    onClick={() => setFileFilter("docs")}
                  >
                    Documents
                  </span>
                  <span
                    className={`pill ${fileFilter === "code" ? "active" : ""}`}
                    onClick={() => setFileFilter("code")}
                  >
                    Source Code
                  </span>
                </div>
              </div>

              <div className="file-table-container">
                <table className="file-table">
                  <thead>
                    <tr>
                      <th style={{ width: 34 }}>
                        <input
                          type="checkbox"
                          checked={filteredItems.length > 0 && selectedPaths.size === filteredItems.length}
                          onChange={(e) => {
                            if (e.target.checked) {
                              setSelectedPaths(new Set(filteredItems.map((i) => i.path)));
                            } else {
                              setSelectedPaths(new Set());
                            }
                          }}
                        />
                      </th>
                      <th onClick={() => { setSortField("name"); setSortAsc(!sortAsc); }} style={{ cursor: "pointer" }}>
                        Name {sortField === "name" && (sortAsc ? "▲" : "▼")}
                      </th>
                      <th onClick={() => { setSortField("size"); setSortAsc(!sortAsc); }} style={{ cursor: "pointer", width: 110 }}>
                        Size {sortField === "size" && (sortAsc ? "▲" : "▼")}
                      </th>
                      <th onClick={() => { setSortField("modified"); setSortAsc(!sortAsc); }} style={{ cursor: "pointer", width: 170 }}>
                        Modified {sortField === "modified" && (sortAsc ? "▲" : "▼")}
                      </th>
                      <th style={{ width: 150 }}>SHA-256</th>
                      <th style={{ width: 100, textAlign: "right" }}>Actions</th>
                    </tr>
                  </thead>
                  <tbody>
                    {loadingFiles ? (
                      <tr>
                        <td colSpan={6} style={{ textAlign: "center", padding: "40px", color: "var(--text-muted)" }}>
                          Loading files from C++ backend...
                        </td>
                      </tr>
                    ) : filteredItems.length === 0 ? (
                      <tr>
                        <td colSpan={6} style={{ textAlign: "center", padding: "50px", color: "var(--text-muted)" }}>
                          Folder is empty. Create a file or click "Reset Demo Workspace" to begin.
                        </td>
                      </tr>
                    ) : (
                      filteredItems.map((item) => {
                        const isSelected = selectedPaths.has(item.path);
                        const isInspected = inspectedItem?.path === item.path;
                        return (
                          <tr
                            key={item.path}
                            className={`file-row ${isSelected || isInspected ? "selected" : ""}`}
                            onClick={() => setInspectedItem(item)}
                            onDoubleClick={() => {
                              if (item.is_directory) {
                                loadDirectory(item.path);
                              } else {
                                openInEditor(item.path);
                              }
                            }}
                          >
                            <td>
                              <input
                                type="checkbox"
                                checked={isSelected}
                                onClick={(e) => e.stopPropagation()}
                                onChange={(e) => {
                                  const next = new Set(selectedPaths);
                                  if (e.target.checked) next.add(item.path);
                                  else next.delete(item.path);
                                  setSelectedPaths(next);
                                }}
                              />
                            </td>
                            <td>
                              <div className="file-name-cell">
                                {item.is_directory ? (
                                  <Folder size={16} color="#b45309" />
                                ) : (
                                  <FileText size={16} color="#1b4d3e" />
                                )}
                                <span>{item.name}</span>
                              </div>
                            </td>
                            <td className="file-size-cell">
                              {item.is_directory ? "-" : formatBytes(item.size_bytes)}
                            </td>
                            <td className="file-date-cell">
                              {formatDate(item.modified_at)}
                            </td>
                            <td className="file-size-cell" style={{ fontSize: 10 }}>
                              {item.hash ? item.hash.substring(0, 14) + "..." : "-"}
                            </td>
                            <td style={{ textAlign: "right" }}>
                              <div style={{ display: "inline-flex", gap: 4 }}>
                                {!item.is_directory && (
                                  <button
                                    className="btn btn-sm btn-icon"
                                    title="Open in Document Editor"
                                    onClick={(e) => {
                                      e.stopPropagation();
                                      openInEditor(item.path);
                                    }}
                                  >
                                    <Edit2 size={12} />
                                  </button>
                                )}
                                <button
                                  className="btn btn-sm btn-icon"
                                  title="Rename (F2)"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    openRenameModal(item);
                                  }}
                                >
                                  <Edit3 size={12} />
                                </button>
                                <button
                                  className="btn btn-sm btn-icon"
                                  title="View File History"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    openFileHistory(item.path);
                                  }}
                                >
                                  <History size={12} />
                                </button>
                                <button
                                  className="btn btn-sm btn-icon btn-danger"
                                  title="Quarantine File"
                                  onClick={(e) => {
                                    e.stopPropagation();
                                    openQuarantineModal([item]);
                                  }}
                                >
                                  <Archive size={12} />
                                </button>
                              </div>
                            </td>
                          </tr>
                        );
                      })
                    )}
                  </tbody>
                </table>
              </div>

              {/* ── Floating Batch Action Toolbar ── */}
              {selectedPaths.size > 0 && (
                <div className="batch-floating-bar">
                  <div className="batch-floating-count">
                    <Check size={14} color="var(--accent-gold)" />
                    <span>{selectedPaths.size} item{selectedPaths.size > 1 ? "s" : ""} selected</span>
                  </div>
                  <button
                    className="btn btn-sm"
                    onClick={() => setBatchMoveModal({ open: true, targetDir: currentPath })}
                    style={{ display: "inline-flex", alignItems: "center", gap: 5 }}
                  >
                    <Move size={12} />
                    <span>Move To...</span>
                  </button>
                  <button
                    className="btn btn-sm btn-danger"
                    onClick={() => {
                      const selected = (listing?.items || []).filter((i) => selectedPaths.has(i.path));
                      openQuarantineModal(selected);
                    }}
                    style={{ display: "inline-flex", alignItems: "center", gap: 5 }}
                  >
                    <Archive size={12} />
                    <span>Quarantine Selected</span>
                  </button>
                  <button
                    className="btn btn-sm"
                    onClick={() => setSelectedPaths(new Set())}
                    style={{ color: "var(--text-muted)" }}
                  >
                    Deselect All
                  </button>
                </div>
              )}
            </div>
          )}

          {/* ──────────────── RIGHT INSPECTOR PANEL ──────────────── */}
          {activeTab === "files" && inspectedItem && (
            <aside className="inspector-panel">
              <div className="inspector-header">
                <div className="inspector-title">Item Details</div>
                <button
                  className="btn-icon"
                  onClick={() => setInspectedItem(null)}
                  style={{ background: "transparent", border: "none" }}
                >
                  <X size={14} />
                </button>
              </div>

              <div className="meta-group">
                <div style={{ fontWeight: 700, fontSize: 13, wordBreak: "break-all" }}>
                  {inspectedItem.name}
                </div>
                <div style={{ fontSize: 11, color: "var(--text-muted)", wordBreak: "break-all", fontFamily: "var(--font-mono)" }}>
                  {inspectedItem.path}
                </div>
              </div>

              <div className="meta-group">
                <div className="meta-row">
                  <span className="meta-label">Type</span>
                  <span className="meta-val">{inspectedItem.is_directory ? "Directory" : `${inspectedItem.extension.toUpperCase() || "File"} document`}</span>
                </div>
                <div className="meta-row">
                  <span className="meta-label">Size</span>
                  <span className="meta-val">{formatBytes(inspectedItem.size_bytes)} ({inspectedItem.size_bytes} bytes)</span>
                </div>
                <div className="meta-row">
                  <span className="meta-label">Modified</span>
                  <span className="meta-val">{formatDate(inspectedItem.modified_at)}</span>
                </div>
                {inspectedItem.hash && (
                  <div className="meta-row" style={{ flexDirection: "column", gap: 2 }}>
                    <span className="meta-label">SHA-256 Checksum</span>
                    <span className="meta-val" style={{ fontSize: 10 }}>{inspectedItem.hash}</span>
                  </div>
                )}
              </div>

              <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
                {!inspectedItem.is_directory && (
                  <>
                    <button
                      className="btn btn-primary"
                      onClick={() => openInEditor(inspectedItem.path)}
                    >
                      <Edit2 size={13} />
                      <span>Open in Document Editor</span>
                    </button>
                    <a
                      className="btn"
                      href={api.getDownloadUrl(inspectedItem.path)}
                      download
                    >
                      <Download size={13} />
                      <span>Download File</span>
                    </a>
                  </>
                )}
                <button
                  className="btn"
                  title="Rename (F2)"
                  onClick={() => openRenameModal(inspectedItem)}
                >
                  <Edit3 size={13} />
                  <span>Rename Item (F2)</span>
                </button>
                <button
                  className="btn"
                  onClick={() => openFileHistory(inspectedItem.path)}
                >
                  <History size={13} />
                  <span>Inspect Revisions & Identity</span>
                </button>
              </div>

              {/* Preview */}
              {!inspectedItem.is_directory && (
                <div className="meta-group" style={{ flex: 1, display: "flex", flexDirection: "column" }}>
                  <div className="meta-label" style={{ marginBottom: 4 }}>Content Preview</div>
                  <pre
                    style={{
                      flex: 1,
                      backgroundColor: "#fff",
                      border: "1px solid var(--border-default)",
                      borderRadius: "var(--radius-sm)",
                      padding: 8,
                      fontFamily: "var(--font-mono)",
                      fontSize: 11,
                      overflow: "auto",
                      maxHeight: 260,
                      whiteSpace: "pre-wrap"
                    }}
                  >
                    {loadingContent ? "Loading preview..." : inspectedContent || "(Empty)"}
                  </pre>
                </div>
              )}
            </aside>
          )}

          {/* ──────────────── 2. DOCUMENT EDITOR VIEW ──────────────── */}
          {activeTab === "editor" && (
            <div className="editor-view">
              <div className="editor-toolbar">
                <div className="editor-file-title">
                  <FileText size={16} color="#1b4d3e" />
                  <span>{editorPath ? pathBasename(editorPath) : "No file loaded"}</span>
                  <span
                    className={`editor-status-pill ${
                      editorStatus === "conflict" ? "conflict" : editorDirty ? "unsaved" : "saved"
                    }`}
                  >
                    {editorStatus === "conflict"
                      ? "External Conflict Detected"
                      : editorDirty
                      ? "Unsaved Changes"
                      : "Saved (Clean)"}
                  </span>
                </div>

                <div style={{ display: "flex", gap: 8 }}>
                  <button
                    className="btn btn-primary"
                    disabled={editorSaving || !editorDirty}
                    onClick={() => handleSaveDocument(false)}
                  >
                    <Save size={13} />
                    <span>{editorSaving ? "Saving..." : "Save (Pre-hash Checked)"}</span>
                  </button>

                  <button
                    className="btn"
                    onClick={() => openFileHistory(editorPath)}
                  >
                    <History size={13} />
                    <span>Revisions</span>
                  </button>

                  <button
                    className="btn"
                    onClick={() => {
                      if (editorDirty && !confirm("Discard unsaved changes?")) return;
                      setActiveTab("files");
                    }}
                  >
                    <span>Close Editor</span>
                  </button>
                </div>
              </div>

              <div className="editor-textarea-container">
                <div className="editor-line-numbers">
                  {editorContent.split("\n").map((_, i) => (
                    <div key={i}>{i + 1}</div>
                  ))}
                </div>
                <textarea
                  className="editor-textarea"
                  value={editorContent}
                  onChange={(e) => {
                    setEditorContent(e.target.value);
                    setEditorDirty(true);
                    setEditorStatus("unsaved");
                  }}
                  placeholder="Type or paste content here..."
                />
              </div>
            </div>
          )}

          {/* ──────────────── 3. FOLDER INSIGHTS & DUPLICATES VIEW ──────────────── */}
          {activeTab === "insights" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <BarChart2 size={16} color="#1b4d3e" />
                    <span>Folder Analytics & Duplicate Detection</span>
                  </div>
                  <div style={{ display: "flex", gap: 8 }}>
                    <button
                      className="btn btn-sm"
                      onClick={() => loadInsights()}
                      disabled={loadingInsights}
                    >
                      <RefreshCw size={12} />
                      <span>{loadingInsights ? "Scanning..." : "Rescan Current Folder"}</span>
                    </button>
                  </div>
                </div>

                {folderInsights && (
                  <>
                    <div style={{ fontSize: 11.5, color: "var(--text-muted)", fontFamily: "var(--font-mono)" }}>
                      Target: {folderInsights.folder_path}
                    </div>

                    <div className="stats-grid">
                      <div className="stat-box">
                        <div className="stat-label">Total Files</div>
                        <div className="stat-value">{folderInsights.total_files}</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Directories</div>
                        <div className="stat-value">{folderInsights.total_directories}</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Total Space</div>
                        <div className="stat-value">{formatBytes(folderInsights.total_bytes)}</div>
                      </div>
                      <div className="stat-box" style={{ borderColor: folderInsights.reclaimable_bytes > 0 ? "var(--warning)" : "var(--border-subtle)" }}>
                        <div className="stat-label">Reclaimable Space (Duplicates)</div>
                        <div className="stat-value" style={{ color: folderInsights.reclaimable_bytes > 0 ? "var(--warning)" : "var(--success)" }}>
                          {formatBytes(folderInsights.reclaimable_bytes)}
                        </div>
                      </div>
                    </div>
                  </>
                )}
              </div>

              {/* Exact Duplicates Table */}
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <CopyIcon size={16} color="#b45309" />
                    <span>Exact Duplicate Files (SHA-256 Byte Matches)</span>
                  </div>
                  {folderInsights && folderInsights.duplicates.length > 0 && (
                    <span className="nav-badge warning">{folderInsights.duplicates.length} Groups Found</span>
                  )}
                </div>

                {folderInsights && folderInsights.duplicates.length > 0 ? (
                  <div style={{ display: "flex", flexDirection: "column", gap: 12 }}>
                    {folderInsights.duplicates.map((group, idx) => (
                      <div
                        key={group.sha256_hash}
                        style={{
                          backgroundColor: "#fffdfa",
                          border: "1px solid var(--border-default)",
                          borderRadius: "var(--radius-sm)",
                          padding: 12
                        }}
                      >
                        <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 6 }}>
                          <div>
                            <span style={{ fontWeight: 700, fontSize: 12 }}>Duplicate Group #{idx + 1}</span>
                            <span style={{ marginLeft: 8, fontSize: 11, color: "var(--text-muted)" }}>
                              Size: {formatBytes(group.size_bytes)} | SHA-256: {group.sha256_hash.substring(0, 16)}...
                            </span>
                          </div>
                          <span style={{ fontWeight: 600, color: "var(--warning)", fontSize: 11 }}>
                            Waste: {formatBytes(group.reclaimable_bytes)}
                          </span>
                        </div>

                        <div style={{ display: "flex", flexDirection: "column", gap: 4 }}>
                          {group.paths.map((p, pIdx) => (
                            <div
                              key={p}
                              style={{
                                display: "flex",
                                justifyContent: "space-between",
                                alignItems: "center",
                                fontSize: 11.5,
                                fontFamily: "var(--font-mono)",
                                backgroundColor: pIdx === 0 ? "rgba(45, 106, 79, 0.08)" : "transparent",
                                padding: "3px 6px",
                                borderRadius: 3
                              }}
                            >
                              <span>{p} {pIdx === 0 && <strong style={{ color: "var(--accent)" }}>(Keep primary)</strong>}</span>
                              {pIdx > 0 && (
                                <button
                                  className="btn btn-sm btn-danger"
                                  onClick={async () => {
                                    if (confirm(`Quarantine duplicate ${pathBasename(p)}?`)) {
                                      await api.deleteItem(p);
                                      showToast("Duplicate quarantined", "info");
                                      loadInsights();
                                      loadDirectory(currentPath);
                                    }
                                  }}
                                >
                                  Remove
                                </button>
                              )}
                            </div>
                          ))}
                        </div>
                      </div>
                    ))}
                  </div>
                ) : (
                  <div style={{ padding: 24, textAlign: "center", color: "var(--text-muted)" }}>
                    No exact byte duplicates detected in this folder.
                  </div>
                )}
              </div>

              {/* Largest Files & Empty Folders */}
              <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 16 }}>
                <div className="card">
                  <div className="card-header">
                    <div className="card-title">Top Largest Files</div>
                  </div>
                  <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
                    {folderInsights?.largest_files.map((lf) => (
                      <div key={lf.path} style={{ display: "flex", justifyContent: "space-between", fontSize: 12 }}>
                        <span style={{ fontFamily: "var(--font-mono)" }}>{lf.name}</span>
                        <span style={{ fontWeight: 600 }}>{formatBytes(lf.size_bytes)}</span>
                      </div>
                    ))}
                  </div>
                </div>

                <div className="card">
                  <div className="card-header">
                    <div className="card-title">Empty Directories</div>
                  </div>
                  <div style={{ display: "flex", flexDirection: "column", gap: 6 }}>
                    {folderInsights && folderInsights.empty_directories.length > 0 ? (
                      folderInsights.empty_directories.map((ed) => (
                        <div key={ed} style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: 12 }}>
                          <span style={{ fontFamily: "var(--font-mono)" }}>{pathBasename(ed)}</span>
                          <button
                            className="btn btn-sm btn-danger"
                            onClick={async () => {
                              await api.deleteItem(ed);
                              showToast("Pruned empty folder", "info");
                              loadInsights();
                            }}
                          >
                            Prune
                          </button>
                        </div>
                      ))
                    ) : (
                      <div style={{ color: "var(--text-muted)", fontSize: 12 }}>No empty directories found.</div>
                    )}
                  </div>
                </div>
              </div>
            </div>
          )}

          {/* ──────────────── 4. EXTERNAL CHANGES INBOX VIEW ──────────────── */}
          {activeTab === "external" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <Inbox size={16} color="#b45309" />
                    <span>External Changes Inbox (Conflict-Aware Candidate Triage)</span>
                  </div>
                  <div style={{ display: "flex", gap: 8 }}>
                    {["PENDING", "ACCEPTED", "REJECTED", "RECOVERED", "ALL"].map((st) => (
                      <span
                        key={st}
                        className={`pill ${externalStatusFilter === st ? "active" : ""}`}
                        onClick={() => setExternalStatusFilter(st)}
                      >
                        {st}
                      </span>
                    ))}
                  </div>
                </div>

                <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                  When external editors or processes modify files inside monitored roots, FileGuard automatically
                  freezes them as candidates (e.g. <code>X-0001</code>) and snapshots their contents. You can review diffs,
                  accept them into official revision history, reject and revert to the prior state, or recover rejected
                  candidates without losing subsequent edits.
                </div>
              </div>

              {externalCandidates.length === 0 ? (
                <div className="card" style={{ padding: 40, textAlign: "center", color: "var(--text-muted)" }}>
                  No external changes matching filter "{externalStatusFilter}".
                </div>
              ) : (
                <div style={{ display: "flex", flexDirection: "column", gap: 14 }}>
                  {externalCandidates.map((cand) => (
                    <div
                      key={cand.id}
                      className={`candidate-card ${
                        cand.status === "ACCEPTED" ? "accepted" : cand.status === "REJECTED" ? "rejected" : ""
                      }`}
                    >
                      <div className="candidate-header">
                        <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
                          <span className="candidate-id">{cand.id}</span>
                          {cand.change_type === "EXTERNAL_RENAME" ? (
                            <span className="candidate-path">
                              <span style={{ color: "var(--text-muted)" }}>{cand.old_path || "(Original)"}</span>
                              <span style={{ margin: "0 6px", color: "var(--warning)" }}>→</span>
                              <span>{cand.path}</span>
                            </span>
                          ) : (
                            <span className="candidate-path">{cand.path}</span>
                          )}
                          <span className={`nav-badge ${cand.change_type === "EXTERNAL_RENAME" ? "warning" : ""}`} style={{ fontSize: 10 }}>
                            {cand.change_type}
                          </span>
                        </div>
                        <span style={{ fontSize: 11, color: "var(--text-muted)" }}>{formatDate(cand.detected_at)}</span>
                      </div>

                      <div style={{ display: "flex", justifyContent: "space-between", fontSize: 11.5 }}>
                        <div>
                          <strong>Candidate Hash:</strong>{" "}
                          <span style={{ fontFamily: "var(--font-mono)" }}>{cand.candidate_hash ? cand.candidate_hash.substring(0, 16) + "..." : "-"}</span>{" "}
                          ({formatBytes(cand.candidate_size)})
                        </div>
                        <div>
                          <strong>Prior Accepted:</strong>{" "}
                          <span style={{ fontFamily: "var(--font-mono)" }}>
                            {cand.prior_accepted_hash ? cand.prior_accepted_hash.substring(0, 16) + "..." : "(None)"}
                          </span>
                        </div>
                      </div>

                      {cand.resolution_notes && (
                        <div style={{ fontSize: 11.5, color: "var(--text-secondary)", fontStyle: "italic" }}>
                          Notes: {cand.resolution_notes}
                        </div>
                      )}

                      <div style={{ display: "flex", justifyContent: "flex-end", gap: 8, marginTop: 4 }}>
                        <button
                          className="btn btn-sm"
                          onClick={async () => {
                            try {
                              const diff = await api.getExternalChangeDiff(cand.id);
                              setSelectedCandidateDiff(diff);
                              setSelectedCandidateItem(cand);
                              setDiffModalOpen(true);
                            } catch (e: any) {
                              showToast(e.message, "error");
                            }
                          }}
                        >
                          <Eye size={12} />
                          <span>View Diff</span>
                        </button>

                        {cand.status === "PENDING" && (
                          <>
                            <button
                              className="btn btn-sm btn-primary"
                              onClick={async () => {
                                await api.acceptExternalChange(cand.id);
                                showToast(`Accepted external change ${cand.id}`, "info");
                                loadExternalChanges();
                                checkHealth();
                              }}
                            >
                              <Check size={12} />
                              <span>Accept Revision</span>
                            </button>

                            <button
                              className="btn btn-sm btn-danger"
                              onClick={async () => {
                                const isRename = cand.change_type === "EXTERNAL_RENAME";
                                await api.rejectExternalChange(cand.id);
                                showToast(
                                  isRename
                                    ? `Rejected external rename: restored filename back to ${cand.old_path || cand.path}`
                                    : `Rejected external modification: restored prior accepted version`,
                                  "info"
                                );
                                loadExternalChanges();
                                checkHealth();
                                loadDirectory(currentPath);
                              }}
                            >
                              <X size={12} />
                              <span>{cand.change_type === "EXTERNAL_RENAME" ? "Reject & Restore Name" : "Reject & Revert"}</span>
                            </button>
                          </>
                        )}

                        {cand.status === "REJECTED" && (
                          <>
                            <button
                              className="btn btn-sm"
                              onClick={async () => {
                                await api.recoverExternalChange(cand.id, "new_copy");
                                showToast("Recovered candidate as separate file copy", "info");
                                loadExternalChanges();
                                loadDirectory(currentPath);
                              }}
                            >
                              Recover as Copy
                            </button>
                            <button
                              className="btn btn-sm btn-primary"
                              onClick={async () => {
                                await api.recoverExternalChange(cand.id, "new_revision");
                                showToast("Applied candidate as new revision on top of work", "info");
                                loadExternalChanges();
                                loadDirectory(currentPath);
                              }}
                            >
                              Recover as Revision
                            </button>
                          </>
                        )}
                      </div>
                    </div>
                  ))}
                </div>
              )}
            </div>
          )}

          {/* ──────────────── 5. ACTIVITY STREAM VIEW ──────────────── */}
          {activeTab === "activity" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <Activity size={16} color="#1b4d3e" />
                    <span>Real-Time OS Event Monitor</span>
                  </div>
                  <div style={{ display: "flex", gap: 8 }}>
                    <span
                      className={`pill ${eventFilter === "all" ? "active" : ""}`}
                      onClick={() => setEventFilter("all")}
                    >
                      All
                    </span>
                    <span
                      className={`pill ${eventFilter === "external" ? "active" : ""}`}
                      onClick={() => setEventFilter("external")}
                    >
                      External Only
                    </span>
                    <span
                      className={`pill ${eventFilter === "app" ? "active" : ""}`}
                      onClick={() => setEventFilter("app")}
                    >
                      Internal Only
                    </span>
                  </div>
                </div>

                <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                  Backend: <strong>{watcherStatus?.backend_type || "Kernel event notifications"}</strong> | Captured:{" "}
                  <strong>{watcherStatus?.events_captured || 0}</strong> events
                </div>
              </div>

              <div className="card" style={{ padding: 0 }}>
                <table className="file-table">
                  <thead>
                    <tr>
                      <th style={{ width: 90 }}>Kind</th>
                      <th>Path</th>
                      <th style={{ width: 120 }}>Origin</th>
                      <th style={{ width: 180 }}>Timestamp</th>
                    </tr>
                  </thead>
                  <tbody>
                    {events
                      .filter((ev) => {
                        if (eventFilter === "external") return ev.is_external;
                        if (eventFilter === "app") return !ev.is_external;
                        return true;
                      })
                      .map((ev, i) => (
                        <tr key={i}>
                          <td>
                            <span
                              className="nav-badge"
                              style={{
                                backgroundColor:
                                  ev.kind === "CREATE"
                                    ? "var(--success-muted)"
                                    : ev.kind === "DELETE"
                                    ? "var(--danger-muted)"
                                    : "var(--warning-muted)",
                                color:
                                  ev.kind === "CREATE"
                                    ? "var(--success)"
                                    : ev.kind === "DELETE"
                                    ? "var(--danger)"
                                    : "var(--warning)"
                              }}
                            >
                              {ev.kind}
                            </span>
                          </td>
                          <td style={{ fontFamily: "var(--font-mono)", fontSize: 12 }}>{ev.path}</td>
                          <td>
                            <span
                              className="nav-badge"
                              style={{
                                backgroundColor: ev.is_external ? "var(--warning-muted)" : "var(--accent-light)",
                                color: ev.is_external ? "var(--warning)" : "var(--accent)"
                              }}
                            >
                              {ev.is_external ? "External" : "FileGuard App"}
                            </span>
                          </td>
                          <td style={{ fontSize: 11.5, color: "var(--text-muted)" }}>{formatDate(ev.timestamp)}</td>
                        </tr>
                      ))}
                  </tbody>
                </table>
              </div>
            </div>
          )}

          {/* ──────────────── 6. BATCH TRANSACTIONS VIEW ──────────────── */}
          {activeTab === "transactions" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <Layers size={16} color="var(--accent)" />
                    <span>Transactional Multi-Operation Batches</span>
                  </div>
                  <button
                    className="btn btn-sm"
                    onClick={() => api.getTransactions().then(setTransactions)}
                    style={{ display: "inline-flex", alignItems: "center", gap: 5 }}
                  >
                    <RefreshCw size={12} />
                    <span>Refresh</span>
                  </button>
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                  Multi-file operations (batch delete, batch move, batch rename) execute inside durable transactions.
                  If any single step encounters a failure, the transaction automatically rolls back and compensates
                  all prior steps from quarantine. You can also manually rollback any completed transaction.
                </div>
              </div>

              {transactions.length === 0 ? (
                <div className="card" style={{ textAlign: "center", padding: 36, color: "var(--text-muted)" }}>
                  No batch transactions recorded yet. Select multiple files in the File Explorer to perform a batch move or batch quarantine.
                </div>
              ) : (
                transactions.map((tx) => (
                  <div key={tx.id} className="card">
                    <div className="card-header">
                      <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                        <span className="badge-gold">{tx.id}</span>
                        <span style={{ fontWeight: 700, fontSize: 13 }}>{tx.type}</span>
                      </div>
                      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
                        <span
                          className="nav-badge"
                          style={{
                            backgroundColor:
                              tx.status === "COMPLETED"
                                ? "var(--success-muted)"
                                : tx.status === "ROLLED_BACK"
                                ? "var(--warning-muted)"
                                : tx.status === "FAILED"
                                ? "var(--danger-muted)"
                                : "var(--accent-light)",
                            color:
                              tx.status === "COMPLETED"
                                ? "var(--success)"
                                : tx.status === "ROLLED_BACK"
                                ? "var(--warning)"
                                : tx.status === "FAILED"
                                ? "var(--danger)"
                                : "var(--accent)",
                            fontSize: 11,
                            padding: "3px 8px"
                          }}
                        >
                          {tx.status}
                        </span>

                        {tx.status !== "ROLLED_BACK" ? (
                          <button
                            className="btn btn-sm btn-warning"
                            onClick={() => handleUndoTransaction(tx.id)}
                            style={{ display: "inline-flex", alignItems: "center", gap: 5 }}
                            title="Undo all operations in this batch transaction"
                          >
                            <RotateCcw size={12} />
                            <span>Rollback Batch</span>
                          </button>
                        ) : (
                          <button
                            className="btn btn-sm btn-outline"
                            onClick={() => handleRedoTransaction(tx.id)}
                            style={{ display: "inline-flex", alignItems: "center", gap: 5 }}
                            title="Reapply all operations in this batch transaction"
                          >
                            <RotateCw size={12} />
                            <span>Reapply Batch</span>
                          </button>
                        )}
                      </div>
                    </div>

                    <div style={{ fontSize: 11.5, color: "var(--text-muted)", display: "flex", gap: 16, marginTop: 4 }}>
                      <span>Progress: <strong>{tx.completed_ops} / {tx.total_ops}</strong> steps completed</span>
                      <span>Created: {formatDate(tx.created_at)}</span>
                      {tx.completed_at && <span>Completed: {formatDate(tx.completed_at)}</span>}
                    </div>

                    {tx.operations && tx.operations.length > 0 && (
                      <div style={{ marginTop: 10 }}>
                        <div style={{ fontSize: 11, fontWeight: 700, color: "var(--text-muted)", textTransform: "uppercase", letterSpacing: "0.05em", marginBottom: 6 }}>
                          Child Operations ({tx.operations.length})
                        </div>
                        <table className="tx-child-table">
                          <thead>
                            <tr>
                              <th style={{ width: 100 }}>Operation ID</th>
                              <th style={{ width: 90 }}>Type</th>
                              <th>Path / Destination</th>
                              <th style={{ width: 90 }}>Status</th>
                            </tr>
                          </thead>
                          <tbody>
                            {tx.operations.map((op) => (
                              <tr key={op.id}>
                                <td style={{ fontFamily: "var(--font-mono)" }}>{op.id}</td>
                                <td>
                                  <span className="badge-gold" style={{ fontSize: 10 }}>{op.type}</span>
                                </td>
                                <td style={{ fontFamily: "var(--font-mono)", fontSize: 11, wordBreak: "break-all" }}>
                                  {op.source_path && op.dest_path ? `${op.source_path} ➔ ${op.dest_path}` : (op.dest_path || op.source_path)}
                                </td>
                                <td>
                                  <span
                                    className="nav-badge"
                                    style={{
                                      fontSize: 10,
                                      backgroundColor: op.is_undone ? "var(--warning-muted)" : "var(--success-muted)",
                                      color: op.is_undone ? "var(--warning)" : "var(--success)"
                                    }}
                                  >
                                    {op.is_undone ? "UNDONE" : op.status}
                                  </span>
                                </td>
                              </tr>
                            ))}
                          </tbody>
                        </table>
                      </div>
                    )}
                  </div>
                ))
              )}
            </div>
          )}

          {/* ──────────────── 7. QUARANTINE VAULT VIEW ──────────────── */}
          {activeTab === "recovery" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <Archive size={16} color="var(--accent)" />
                    <span>Quarantine Vault (Safe Deletions & Isolation)</span>
                  </div>
                  <button
                    className="btn btn-sm"
                    onClick={() => api.getQuarantine().then(setQuarantineItems)}
                    style={{ display: "inline-flex", alignItems: "center", gap: 5 }}
                  >
                    <RefreshCw size={12} />
                    <span>Refresh Vault</span>
                  </button>
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                  Deleted or quarantined files are never permanently unlinked immediately. They are moved into the encrypted/isolated
                  Quarantine Vault with cryptographic manifests, allowing zero-loss restoration to original or custom paths.
                </div>
              </div>

              <div className="card" style={{ padding: 0 }}>
                <table className="file-table">
                  <thead>
                    <tr>
                      <th>Original Path</th>
                      <th style={{ width: 90 }}>Size</th>
                      <th style={{ width: 140 }}>Quarantined At</th>
                      <th style={{ width: 130 }}>Checksum</th>
                      <th style={{ width: 280, textAlign: "right" }}>Actions</th>
                    </tr>
                  </thead>
                  <tbody>
                    {quarantineItems.length === 0 ? (
                      <tr>
                        <td colSpan={5} style={{ textAlign: "center", padding: 36, color: "var(--text-muted)" }}>
                          Quarantine vault is empty. No isolated items.
                        </td>
                      </tr>
                    ) : (
                      quarantineItems.map((q) => (
                        <tr key={q.id}>
                          <td>
                            <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                              {q.is_directory ? <Folder size={14} color="var(--accent-gold)" /> : <FileText size={14} color="var(--accent)" />}
                              <span style={{ fontFamily: "var(--font-mono)", fontSize: 12 }}>{q.original_path}</span>
                            </div>
                          </td>
                          <td className="file-size-cell">{formatBytes(q.size_bytes)}</td>
                          <td className="file-date-cell">{formatDate(q.quarantined_at)}</td>
                          <td className="file-size-cell" style={{ fontSize: 10 }}>
                            {q.sha256_hash ? q.sha256_hash.substring(0, 12) + "..." : "-"}
                          </td>
                          <td style={{ textAlign: "right" }}>
                            <div style={{ display: "inline-flex", gap: 5 }}>
                              <button
                                className="btn btn-sm btn-primary"
                                title="Restore to original location"
                                onClick={() => handleRestoreQuarantine(q)}
                              >
                                Restore
                              </button>
                              <button
                                className="btn btn-sm"
                                title="Restore to custom location"
                                onClick={() =>
                                  setRestoreCustomModal({
                                    open: true,
                                    item: q,
                                    customDest: q.original_path
                                  })
                                }
                              >
                                Restore to...
                              </button>
                              <button
                                className="btn btn-sm btn-icon"
                                title="Inspect quarantine details"
                                onClick={() => setQuarantineDetailModal(q)}
                              >
                                <Eye size={12} />
                              </button>
                              <button
                                className="btn btn-sm btn-danger btn-icon"
                                title="Permanently delete from vault"
                                onClick={() => setPurgeConfirmModal(q)}
                              >
                                <Trash2 size={12} />
                              </button>
                            </div>
                          </td>
                        </tr>
                      ))
                    )}
                  </tbody>
                </table>
              </div>
            </div>
          )}

          {/* ──────────────── 8. FILE HISTORY & VERSIONS VIEW ──────────────── */}
          {activeTab === "history" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <History size={16} color="#1b4d3e" />
                    <span>Independent File Identity & Revision History</span>
                  </div>
                  <div style={{ display: "flex", gap: 8 }}>
                    <span
                      className={`pill ${activeHistorySubTab === "content_revisions" ? "active" : ""}`}
                      onClick={() => setActiveHistorySubTab("content_revisions")}
                    >
                      Content Revisions ({fileHistory?.content_revisions.length || 0})
                    </span>
                    <span
                      className={`pill ${activeHistorySubTab === "name_location" ? "active" : ""}`}
                      onClick={() => setActiveHistorySubTab("name_location")}
                    >
                      Name & Location History ({fileHistory?.name_location_history.length || 0})
                    </span>
                    <span
                      className={`pill ${activeHistorySubTab === "external_changes" ? "active" : ""}`}
                      onClick={() => setActiveHistorySubTab("external_changes")}
                    >
                      External Changes ({fileHistory?.external_candidates?.length || 0})
                    </span>
                    <span
                      className={`pill ${activeHistorySubTab === "operations" ? "active" : ""}`}
                      onClick={() => setActiveHistorySubTab("operations")}
                    >
                      Operations Journal ({fileHistory?.operations.length || 0})
                    </span>
                  </div>
                </div>

                <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
                  <input
                    type="text"
                    style={{
                      flex: 1,
                      padding: "6px 10px",
                      border: "1px solid var(--border-default)",
                      borderRadius: "var(--radius-sm)",
                      fontFamily: "var(--font-mono)",
                      fontSize: 12
                    }}
                    value={historyTarget}
                    onChange={(e) => setHistoryTarget(e.target.value)}
                    placeholder="Enter file path to inspect..."
                  />
                  <button className="btn btn-primary" onClick={() => openFileHistory(historyTarget)}>
                    Inspect
                  </button>
                </div>

                {fileHistory?.file_identity && (
                  <div className="stat-box" style={{ marginTop: 8 }}>
                    <div style={{ display: "flex", justifyContent: "space-between" }}>
                      <div>
                        <strong>File Identity (FID):</strong>{" "}
                        <span style={{ fontFamily: "var(--font-mono)", color: "var(--accent)" }}>
                          {fileHistory.file_identity.id}
                        </span>
                      </div>
                      <div style={{ fontSize: 11, color: "var(--text-muted)" }}>
                        First created: {formatDate(fileHistory.file_identity.created_at)}
                      </div>
                    </div>
                  </div>
                )}
              </div>

              {activeHistorySubTab === "content_revisions" && (
                <div className="card" style={{ padding: 0 }}>
                  <table className="file-table">
                    <thead>
                      <tr>
                        <th style={{ width: 90 }}>Rev ID</th>
                        <th style={{ width: 80 }}>Rev #</th>
                        <th style={{ width: 140 }}>Origin</th>
                        <th>Summary</th>
                        <th style={{ width: 100 }}>Size</th>
                        <th style={{ width: 160 }}>Created</th>
                        <th style={{ width: 100, textAlign: "right" }}>Action</th>
                      </tr>
                    </thead>
                    <tbody>
                      {fileHistory?.content_revisions.map((rev) => (
                        <tr key={rev.id}>
                          <td style={{ fontFamily: "var(--font-mono)", fontWeight: 600 }}>{rev.id}</td>
                          <td style={{ fontWeight: 700 }}>Rev {rev.revision_number}</td>
                          <td>
                            <span className="nav-badge" style={{ fontSize: 10 }}>{rev.source_type}</span>
                          </td>
                          <td>{rev.change_summary}</td>
                          <td className="file-size-cell">{formatBytes(rev.size_bytes)}</td>
                          <td className="file-date-cell">{formatDate(rev.created_at)}</td>
                          <td style={{ textAlign: "right" }}>
                            {rev.snapshot_id > 0 && (
                              <button
                                className="btn btn-sm btn-primary"
                                onClick={async () => {
                                  if (confirm(`Restore to ${rev.id} (Rev ${rev.revision_number})?`)) {
                                    await api.restoreRevision(rev.snapshot_id, rev.path);
                                    showToast("Revision restored", "info");
                                    openFileHistory(rev.path);
                                  }
                                }}
                              >
                                Restore
                              </button>
                            )}
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}

              {activeHistorySubTab === "name_location" && (
                <div className="card" style={{ padding: 0 }}>
                  <table className="file-table">
                    <thead>
                      <tr>
                        <th style={{ width: 90 }}>Op ID</th>
                        <th style={{ width: 90 }}>Type</th>
                        <th>Source Path</th>
                        <th>Destination Path</th>
                        <th style={{ width: 160 }}>Date</th>
                      </tr>
                    </thead>
                    <tbody>
                      {fileHistory?.name_location_history.map((nl) => (
                        <tr key={nl.id}>
                          <td style={{ fontFamily: "var(--font-mono)" }}>{nl.id}</td>
                          <td>
                            <span className="nav-badge">{nl.op_type}</span>
                          </td>
                          <td style={{ fontFamily: "var(--font-mono)", fontSize: 11 }}>{nl.source_path}</td>
                          <td style={{ fontFamily: "var(--font-mono)", fontSize: 11 }}>{nl.dest_path}</td>
                          <td className="file-date-cell">{formatDate(nl.created_at)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}

              {activeHistorySubTab === "external_changes" && (
                <div className="card" style={{ padding: 0 }}>
                  {(!fileHistory?.external_candidates || fileHistory.external_candidates.length === 0) ? (
                    <div style={{ padding: 32, textAlign: "center", color: "var(--text-muted)" }}>
                      <Shield size={32} style={{ margin: "0 auto 12px", opacity: 0.4 }} />
                      <div style={{ fontWeight: 600, fontSize: 13 }}>No External Changes Recorded</div>
                      <div style={{ fontSize: 11.5, marginTop: 4 }}>
                        Any modifications, renames, or overwrites made to this file identity outside FileGuard will appear here across all renames.
                      </div>
                    </div>
                  ) : (
                    <table className="file-table">
                      <thead>
                        <tr>
                          <th style={{ width: 85 }}>ID</th>
                          <th style={{ width: 100 }}>Status</th>
                          <th style={{ width: 130 }}>Type</th>
                          <th>Recorded Path / Details</th>
                          <th style={{ width: 90 }}>Size</th>
                          <th style={{ width: 150 }}>Detected</th>
                          <th style={{ width: 170, textAlign: "right" }}>Actions</th>
                        </tr>
                      </thead>
                      <tbody>
                        {fileHistory.external_candidates.map((cand) => (
                          <tr key={cand.id}>
                            <td style={{ fontFamily: "var(--font-mono)", fontWeight: 600 }}>{cand.id}</td>
                            <td>
                              <span
                                className="nav-badge"
                                style={{
                                  fontSize: 10.5,
                                  backgroundColor:
                                    cand.status === "ACCEPTED"
                                      ? "var(--success-muted)"
                                      : cand.status === "REJECTED"
                                      ? "var(--danger-muted)"
                                      : cand.status === "RECOVERED"
                                      ? "rgba(107, 31, 42, 0.15)"
                                      : "var(--warning-muted)",
                                  color:
                                    cand.status === "ACCEPTED"
                                      ? "var(--success)"
                                      : cand.status === "REJECTED"
                                      ? "var(--danger)"
                                      : cand.status === "RECOVERED"
                                      ? "var(--accent)"
                                      : "var(--warning)",
                                  fontWeight: 700,
                                }}
                              >
                                {cand.status}
                              </span>
                            </td>
                            <td>
                              <span style={{ fontSize: 11, fontFamily: "var(--font-mono)" }}>
                                {cand.change_type}
                              </span>
                            </td>
                            <td style={{ fontFamily: "var(--font-mono)", fontSize: 11 }}>
                              {cand.old_path ? `${cand.old_path} → ${cand.path}` : cand.path}
                              {cand.resolution_notes && (
                                <div style={{ fontSize: 11, color: "var(--text-muted)", marginTop: 2 }}>
                                  {cand.resolution_notes}
                                </div>
                              )}
                            </td>
                            <td className="file-size-cell">{formatBytes(cand.candidate_size)}</td>
                            <td className="file-date-cell">{formatDate(cand.detected_at)}</td>
                            <td style={{ textAlign: "right" }}>
                              <div style={{ display: "flex", gap: 6, justifyContent: "flex-end" }}>
                                {cand.candidate_snapshot_path && (
                                  <button
                                    className="btn btn-sm"
                                    title="View Visual Diff"
                                    onClick={() => openDiffForCandidate(cand)}
                                  >
                                    <Eye size={12} />
                                    <span>Diff</span>
                                  </button>
                                )}
                                {cand.status === "PENDING" && (
                                  <>
                                    <button
                                      className="btn btn-sm btn-primary"
                                      title="Accept External Change into History"
                                      onClick={async () => {
                                        await api.acceptExternalChange(cand.id);
                                        showToast(`Accepted external change ${cand.id}`, "info");
                                        checkHealth();
                                        openFileHistory(historyTarget);
                                      }}
                                    >
                                      Accept
                                    </button>
                                    <button
                                      className="btn btn-sm btn-danger"
                                      title="Reject and Restore Protected File"
                                      onClick={async () => {
                                        await api.rejectExternalChange(cand.id);
                                        showToast(`Rejected external change ${cand.id}`, "info");
                                        checkHealth();
                                        openFileHistory(historyTarget);
                                      }}
                                    >
                                      Reject
                                    </button>
                                  </>
                                )}
                                {cand.status === "REJECTED" && (
                                  <button
                                    className="btn btn-sm"
                                    title="Recover Rejected Candidate as Revision"
                                    onClick={async () => {
                                      await api.recoverExternalChange(cand.id, "new_revision");
                                      showToast(`Recovered ${cand.id} as new revision`, "info");
                                      checkHealth();
                                      openFileHistory(historyTarget);
                                    }}
                                  >
                                    Recover
                                  </button>
                                )}
                              </div>
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                </div>
              )}

              {activeHistorySubTab === "operations" && (
                <div className="card" style={{ padding: 0 }}>
                  <table className="file-table">
                    <thead>
                      <tr>
                        <th style={{ width: 90 }}>ID</th>
                        <th style={{ width: 100 }}>Type</th>
                        <th style={{ width: 100 }}>Status</th>
                        <th>Path</th>
                        <th style={{ width: 160 }}>Timestamp</th>
                      </tr>
                    </thead>
                    <tbody>
                      {fileHistory?.operations.map((op) => (
                        <tr key={op.id}>
                          <td style={{ fontFamily: "var(--font-mono)" }}>{op.id}</td>
                          <td>{op.type}</td>
                          <td>
                            <span className="nav-badge">{op.status}</span>
                          </td>
                          <td style={{ fontFamily: "var(--font-mono)", fontSize: 11 }}>{op.source_path || op.dest_path}</td>
                          <td className="file-date-cell">{formatDate(op.created_at)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </div>
          )}

          {/* ──────────────── 9. OS PERFORMANCE LAB VIEW ──────────────── */}
          {activeTab === "benchmarks" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <Sliders size={16} color="#1b4d3e" />
                    <span>OS Performance Laboratory</span>
                  </div>
                  <a className="btn btn-sm" href={api.getBenchmarkCsvUrl()} download="fileguard_benchmarks.csv">
                    <Download size={12} />
                    <span>Export CSV</span>
                  </a>
                </div>
                <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                  Demonstrate core operating system design principles through reproducible, bounded live experiments.
                </div>
              </div>

              {/* Experiment A */}
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <span>Experiment A: Event-Driven Notification vs Periodic Polling</span>
                  </div>
                  <button
                    className="btn btn-primary btn-sm"
                    disabled={runningBench !== null}
                    onClick={async () => {
                      setRunningBench("watcher");
                      try {
                        const res = await api.runBenchmark("watcher", { file_count: 15, polling_interval_ms: 50 });
                        setBenchWatcherResult(res);
                        showToast("Experiment A completed successfully", "info");
                        api.getBenchmarks().then(setBenchmarks);
                      } catch (err: any) {
                        showToast(err.message, "error");
                      } finally {
                        setRunningBench(null);
                      }
                    }}
                  >
                    <Play size={12} />
                    <span>{runningBench === "watcher" ? "Measuring..." : "Run Experiment A (<2s)"}</span>
                  </button>
                </div>

                <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                  Measures latency from stimulus creation to detection between native OS kernel events (Windows IOCP / ReadDirectoryChangesW)
                  and active periodic polling loops.
                </div>

                {benchWatcherResult && (
                  <div style={{ marginTop: 10, display: "flex", flexDirection: "column", gap: 10 }}>
                    <div style={{ fontSize: 12, fontWeight: 600, color: "var(--accent)" }}>
                      Stimulus-to-Detection Latency Profile ({benchWatcherResult.file_count || 15} kernel events):
                    </div>
                    <div className="stats-grid">
                      <div className="stat-box">
                        <div className="stat-label">Event Avg Latency</div>
                        <div className="stat-value">{benchWatcherResult.watcher_avg_latency_ms?.toFixed(2)} ms</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Event Median Latency</div>
                        <div className="stat-value">{benchWatcherResult.watcher_median_latency_ms?.toFixed(2)} ms</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Event 95th % Latency</div>
                        <div className="stat-value">{benchWatcherResult.watcher_p95_latency_ms?.toFixed(2)} ms</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Event Min Latency</div>
                        <div className="stat-value">{benchWatcherResult.watcher_min_latency_ms?.toFixed(2)} ms</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Polling Avg Latency</div>
                        <div className="stat-value" style={{ color: "var(--warning)" }}>
                          {benchWatcherResult.polling_avg_latency_ms?.toFixed(2)} ms
                        </div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Polling Median Latency</div>
                        <div className="stat-value" style={{ color: "var(--warning)" }}>
                          {benchWatcherResult.polling_median_latency_ms?.toFixed(2)} ms
                        </div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Polling 95th % Latency</div>
                        <div className="stat-value" style={{ color: "var(--warning)" }}>
                          {benchWatcherResult.polling_p95_latency_ms?.toFixed(2)} ms
                        </div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Polling Scans Executed</div>
                        <div className="stat-value" style={{ color: "var(--warning)" }}>
                          {benchWatcherResult.polling_scans}
                        </div>
                      </div>
                      <div className="stat-box" style={{ gridColumn: "span 2" }}>
                        <div className="stat-label">Event Speedup Factor</div>
                        <div className="stat-value" style={{ color: "var(--accent)", fontSize: 18 }}>
                          {benchWatcherResult.latency_speedup?.toFixed(1)}x faster response
                        </div>
                      </div>
                    </div>
                    <div style={{ fontSize: 11.5, color: "var(--text-secondary)", fontStyle: "italic", padding: "6px 10px", background: "var(--bg-elevated)", borderRadius: "var(--radius-sm)", border: "1px solid var(--border-subtle)" }}>
                      OS Tradeoff Analysis: {benchWatcherResult.cpu_io_summary || "Kernel async event dispatch eliminates wasted CPU poll cycles and detects changes instantaneously."}
                    </div>
                  </div>
                )}
              </div>

              {/* Experiment B */}
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <span>Experiment B: Recovery Protection Overhead vs Raw Operations</span>
                  </div>
                  <button
                    className="btn btn-primary btn-sm"
                    disabled={runningBench !== null}
                    onClick={async () => {
                      setRunningBench("journal");
                      try {
                        const res = await api.runBenchmark("journal", { ops_count: 50 });
                        setBenchJournalResult(res);
                        showToast("Experiment B completed", "info");
                        api.getBenchmarks().then(setBenchmarks);
                      } catch (err: any) {
                        showToast(err.message, "error");
                      } finally {
                        setRunningBench(null);
                      }
                    }}
                  >
                    <Play size={12} />
                    <span>{runningBench === "journal" ? "Measuring..." : "Run Experiment B"}</span>
                  </button>
                </div>

                {benchJournalResult && (
                  <div className="stats-grid" style={{ marginTop: 10 }}>
                    <div className="stat-box">
                      <div className="stat-label">Raw Throughput</div>
                      <div className="stat-value">{(benchJournalResult.raw_throughput_ops_per_sec || benchJournalResult.raw_throughput)?.toFixed(0)} ops/s</div>
                    </div>
                    <div className="stat-box">
                      <div className="stat-label">Journaled Throughput</div>
                      <div className="stat-value">{(benchJournalResult.journaled_throughput_ops_per_sec || benchJournalResult.journaled_throughput)?.toFixed(0)} ops/s</div>
                    </div>
                    <div className="stat-box">
                      <div className="stat-label">Protection Overhead</div>
                      <div className="stat-value" style={{ color: "var(--warning)" }}>
                        {(benchJournalResult.overhead_percentage ?? benchJournalResult.overhead_pct)?.toFixed(1)}%
                      </div>
                    </div>
                    <div className="stat-box">
                      <div className="stat-label">Crash Recovery Time</div>
                      <div className="stat-value" style={{ color: "var(--accent)" }}>
                        {benchJournalResult.recovery_time_ms?.toFixed(2)} ms
                      </div>
                    </div>
                  </div>
                )}
              </div>

              {/* Experiment C */}
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <span>Experiment C: Sequential vs Concurrent Directory Traversal</span>
                  </div>
                  <button
                    className="btn btn-primary btn-sm"
                    disabled={runningBench !== null}
                    onClick={async () => {
                      setRunningBench("scan");
                      try {
                        const res = await api.runBenchmark("scan", { file_count: 200, threads: 4 });
                        setBenchScanResult(res);
                        showToast("Experiment C completed", "info");
                        api.getBenchmarks().then(setBenchmarks);
                      } catch (err: any) {
                        showToast(err.message, "error");
                      } finally {
                        setRunningBench(null);
                      }
                    }}
                  >
                    <Play size={12} />
                    <span>{runningBench === "scan" ? "Measuring..." : "Run Experiment C"}</span>
                  </button>
                </div>

                {benchScanResult && (
                  <div style={{ marginTop: 10, display: "flex", flexDirection: "column", gap: 10 }}>
                    <div className="stats-grid">
                      <div className="stat-box">
                        <div className="stat-label">Sequential Duration</div>
                        <div className="stat-value">{benchScanResult.seq_elapsed_ms?.toFixed(1)} ms</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Concurrent Duration</div>
                        <div className="stat-value">{benchScanResult.conc_elapsed_ms?.toFixed(1)} ms</div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Speedup Factor</div>
                        <div
                          className="stat-value"
                          style={{
                            color: (benchScanResult.correctness || benchScanResult.correctness_verified)
                              ? "var(--accent)"
                              : "var(--danger)"
                          }}
                        >
                          {(benchScanResult.correctness || benchScanResult.correctness_verified)
                            ? `${benchScanResult.speedup?.toFixed(2)}x`
                            : "Invalidated (Mismatch)"}
                        </div>
                      </div>
                      <div className="stat-box">
                        <div className="stat-label">Correctness Parity</div>
                        <div
                          className="stat-value"
                          style={{
                            color: (benchScanResult.correctness || benchScanResult.correctness_verified)
                              ? "var(--success)"
                              : "var(--danger)",
                            fontSize: 13
                          }}
                        >
                          {(benchScanResult.correctness || benchScanResult.correctness_verified)
                            ? "Verified 100% (Identical)"
                            : "Mismatch (Untrustworthy)"}
                        </div>
                      </div>
                    </div>
                    {!(benchScanResult.correctness || benchScanResult.correctness_verified) && (
                      <div style={{
                        padding: "8px 12px",
                        backgroundColor: "var(--danger-muted)",
                        color: "var(--danger)",
                        borderRadius: "var(--radius-sm)",
                        fontSize: 12,
                        display: "flex",
                        alignItems: "center",
                        gap: 8
                      }}>
                        <AlertTriangle size={15} />
                        <span>
                          <strong>Correctness Mismatch:</strong> Sequential and concurrent traversals found differing item or byte counts. The speedup metric has been invalidated.
                        </span>
                      </div>
                    )}
                    {(benchScanResult.correctness || benchScanResult.correctness_verified) && (
                      <div style={{ fontSize: 11.5, color: "var(--text-secondary)", fontStyle: "italic" }}>
                        Dataset Parity: Sequential and concurrent scans both visited exactly {benchScanResult.seq_files_found || benchScanResult.total_files} files with 100% byte-accurate parity across {benchScanResult.threads || 4} worker threads.
                      </div>
                    )}
                  </div>
                )}
              </div>
            </div>
          )}

          {/* ──────────────── 10. SETTINGS & STORAGE VIEW ──────────────── */}
          {activeTab === "settings" && (
            <div className="grid-view">
              <div className="card">
                <div className="card-header">
                  <div className="card-title">
                    <SettingsIcon size={16} color="#1b4d3e" />
                    <span>System Settings & Storage Engine</span>
                  </div>
                </div>

                <div className="stats-grid">
                  <div className="stat-box">
                    <div className="stat-label">Quarantine Storage</div>
                    <div className="stat-value">{formatBytes(settings?.storage_usage?.quarantine_bytes || 0)}</div>
                  </div>
                  <div className="stat-box">
                    <div className="stat-label">Snapshot Versions</div>
                    <div className="stat-value">{formatBytes(settings?.storage_usage?.snapshots_bytes || 0)}</div>
                  </div>
                  <div className="stat-box">
                    <div className="stat-label">Total Guard Storage</div>
                    <div className="stat-value">{formatBytes(settings?.storage_usage?.total_bytes || 0)}</div>
                  </div>
                </div>

                <div style={{ marginTop: 16 }}>
                  <div style={{ fontWeight: 700, marginBottom: 6 }}>Approved Access Roots</div>
                  <div style={{ display: "flex", flexDirection: "column", gap: 4 }}>
                    {settings?.approved_roots.map((r) => (
                      <div
                        key={r}
                        style={{
                          fontFamily: "var(--font-mono)",
                          fontSize: 12,
                          backgroundColor: "#faf8f5",
                          padding: "4px 8px",
                          borderRadius: 3,
                          border: "1px solid var(--border-subtle)"
                        }}
                      >
                        {r}
                      </div>
                    ))}
                  </div>
                </div>

                <div style={{ marginTop: 16 }}>
                  <button className="btn" onClick={handlePopulateDemo}>
                    Repopulate Demo Workspace (watched/)
                  </button>
                </div>
              </div>
            </div>
          )}
        </div>
      </div>

      {/* ── Visual Diff Modal ──────────────────────── */}
      {diffModalOpen && selectedCandidateDiff && (
        <div className="modal-backdrop" onClick={() => setDiffModalOpen(false)}>
          <div className="modal-dialog" style={{ maxWidth: 900, maxHeight: "90vh", display: "flex", flexDirection: "column" }} onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                <Eye size={16} color="var(--accent)" />
                <span style={{ fontWeight: 700, fontSize: 14 }}>
                  External Modification Diff Viewer
                </span>
                <span className="badge badge-warning" style={{ fontSize: 10 }}>
                  {selectedCandidateDiff.candidate_id}
                </span>
              </div>
              <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                <div style={{ display: "flex", background: "var(--bg-elevated)", border: "1px solid var(--border-subtle)", borderRadius: "var(--radius-sm)", padding: 2 }}>
                  <button
                    className={`btn btn-sm ${diffMode === "unified" ? "btn-primary" : ""}`}
                    style={{ padding: "3px 8px", fontSize: 11, border: "none" }}
                    onClick={() => setDiffMode("unified")}
                  >
                    Unified
                  </button>
                  <button
                    className={`btn btn-sm ${diffMode === "split" ? "btn-primary" : ""}`}
                    style={{ padding: "3px 8px", fontSize: 11, border: "none" }}
                    onClick={() => setDiffMode("split")}
                  >
                    <Columns size={12} style={{ marginRight: 4 }} />
                    Side-by-Side
                  </button>
                </div>
                <button className="btn-icon" onClick={() => setDiffModalOpen(false)} style={{ background: "transparent", border: "none" }}>
                  <X size={16} />
                </button>
              </div>
            </div>

            <div className="modal-body" style={{ flex: 1, overflowY: "auto", display: "flex", flexDirection: "column", gap: 10 }}>
              <div className="diff-banner" style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "8px 12px", background: "var(--bg-elevated)", borderRadius: "var(--radius-sm)", border: "1px solid var(--border-subtle)" }}>
                <div style={{ fontSize: 12, fontFamily: "var(--font-mono)", color: "var(--text-primary)" }}>
                  {selectedCandidateDiff.path}
                </div>
                <div style={{ display: "flex", gap: 12, fontSize: 11 }}>
                  <span style={{ color: "var(--text-muted)" }}>
                    Prior Accepted Hash: <code style={{ color: "var(--text-secondary)" }}>{selectedCandidateDiff.prior_hash ? selectedCandidateDiff.prior_hash.substring(0, 10) : "(empty / none)"}</code>
                  </span>
                  <span style={{ color: "var(--warning)" }}>
                    Incoming External Hash: <code>{selectedCandidateDiff.candidate_hash.substring(0, 10)}</code>
                  </span>
                </div>
              </div>

              {(() => {
                const lines = computeLineDiff(selectedCandidateDiff.prior_content, selectedCandidateDiff.candidate_content);
                const addCount = lines.filter((l) => l.type === "added").length;
                const delCount = lines.filter((l) => l.type === "deleted").length;

                return (
                  <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
                    <div style={{ display: "flex", gap: 8, alignItems: "center", fontSize: 12 }}>
                      <span className="badge" style={{ backgroundColor: "rgba(16, 185, 129, 0.15)", color: "var(--success)", fontWeight: 600 }}>
                        +{addCount} additions
                      </span>
                      <span className="badge" style={{ backgroundColor: "rgba(239, 68, 68, 0.15)", color: "var(--danger)", fontWeight: 600 }}>
                        -{delCount} deletions
                      </span>
                      {selectedCandidateDiff.prior_content === selectedCandidateDiff.candidate_content && (
                        <span style={{ color: "var(--warning)", fontStyle: "italic", marginLeft: 8 }}>
                          Notice: Content bytes are identical between versions.
                        </span>
                      )}
                      {!selectedCandidateDiff.prior_content && (
                        <span style={{ color: "var(--info)", fontStyle: "italic", marginLeft: 8 }}>
                          New file creation: all content is incoming external data.
                        </span>
                      )}
                    </div>

                    {diffMode === "unified" ? (
                      <div className="diff-viewer" style={{ border: "1px solid var(--border-default)", borderRadius: "var(--radius-sm)", overflow: "hidden", maxHeight: 420, overflowY: "auto" }}>
                        <table className="diff-table" style={{ width: "100%", borderCollapse: "collapse", fontFamily: "var(--font-mono)", fontSize: 11.5 }}>
                          <tbody>
                            {lines.length === 0 ? (
                              <tr>
                                <td colSpan={4} style={{ padding: "16px", textAlign: "center", color: "var(--text-muted)" }}>
                                  No content differences detected.
                                </td>
                              </tr>
                            ) : (
                              lines.map((dl, idx) => (
                                <tr
                                  key={idx}
                                  className={
                                    dl.type === "added"
                                      ? "diff-line-added"
                                      : dl.type === "deleted"
                                      ? "diff-line-deleted"
                                      : "diff-line-unchanged"
                                  }
                                >
                                  <td style={{ width: 45, padding: "2px 6px", textAlign: "right", color: "var(--text-muted)", userSelect: "none", borderRight: "1px solid var(--border-subtle)" }}>
                                    {dl.oldLineNum ?? ""}
                                  </td>
                                  <td style={{ width: 45, padding: "2px 6px", textAlign: "right", color: "var(--text-muted)", userSelect: "none", borderRight: "1px solid var(--border-subtle)" }}>
                                    {dl.newLineNum ?? ""}
                                  </td>
                                  <td style={{ width: 24, padding: "2px 4px", textAlign: "center", fontWeight: 700, userSelect: "none", color: dl.type === "added" ? "var(--success)" : dl.type === "deleted" ? "var(--danger)" : "transparent" }}>
                                    {dl.type === "added" ? "+" : dl.type === "deleted" ? "-" : " "}
                                  </td>
                                  <td style={{ padding: "2px 8px", whiteSpace: "pre-wrap", wordBreak: "break-all" }}>
                                    {dl.text || " "}
                                  </td>
                                </tr>
                              ))
                            )}
                          </tbody>
                        </table>
                      </div>
                    ) : (
                      <div className="diff-container" style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 10, maxHeight: 420 }}>
                        <div className="diff-pane" style={{ border: "1px solid var(--border-default)", borderRadius: "var(--radius-sm)", display: "flex", flexDirection: "column", overflow: "hidden" }}>
                          <div className="diff-pane-title" style={{ padding: "6px 10px", background: "var(--bg-elevated)", borderBottom: "1px solid var(--border-subtle)", fontWeight: 600, fontSize: 11 }}>
                            Prior Accepted Baseline ({selectedCandidateDiff.prior_hash ? selectedCandidateDiff.prior_hash.substring(0, 10) : "None"})
                          </div>
                          <pre style={{ margin: 0, padding: 10, flex: 1, overflowY: "auto", fontFamily: "var(--font-mono)", fontSize: 11.5, background: "var(--bg-surface)", color: "var(--text-primary)" }}>
                            {selectedCandidateDiff.prior_content || "(Empty / File did not previously exist)"}
                          </pre>
                        </div>
                        <div className="diff-pane" style={{ border: "1px solid var(--border-default)", borderRadius: "var(--radius-sm)", display: "flex", flexDirection: "column", overflow: "hidden" }}>
                          <div className="diff-pane-title" style={{ padding: "6px 10px", background: "var(--bg-elevated)", borderBottom: "1px solid var(--border-subtle)", fontWeight: 600, fontSize: 11, color: "var(--warning)" }}>
                            Incoming External Candidate ({selectedCandidateDiff.candidate_hash.substring(0, 10)})
                          </div>
                          <pre style={{ margin: 0, padding: 10, flex: 1, overflowY: "auto", fontFamily: "var(--font-mono)", fontSize: 11.5, background: "var(--bg-surface)", color: "var(--text-primary)" }}>
                            {selectedCandidateDiff.candidate_content || "(Empty)"}
                          </pre>
                        </div>
                      </div>
                    )}
                  </div>
                );
              })()}
            </div>

            <div className="modal-footer" style={{ display: "flex", justifyContent: "space-between" }}>
              <div style={{ display: "flex", gap: 8 }}>
                {selectedCandidateItem && selectedCandidateItem.status === "PENDING" && (
                  <>
                    <button
                      className="btn btn-primary btn-sm"
                      onClick={async () => {
                        await api.acceptExternalChange(selectedCandidateDiff.candidate_id);
                        showToast(`Accepted external change ${selectedCandidateDiff.candidate_id}`, "info");
                        setDiffModalOpen(false);
                        loadExternalChanges();
                        checkHealth();
                      }}
                    >
                      <Check size={12} />
                      <span>Accept Revision</span>
                    </button>
                    <button
                      className="btn btn-danger btn-sm"
                      onClick={async () => {
                        await api.rejectExternalChange(selectedCandidateDiff.candidate_id);
                        showToast(`Rejected external change ${selectedCandidateDiff.candidate_id}`, "warning");
                        setDiffModalOpen(false);
                        loadExternalChanges();
                        checkHealth();
                        loadDirectory(currentPath);
                      }}
                    >
                      <X size={12} />
                      <span>Reject & Restore Original</span>
                    </button>
                  </>
                )}
              </div>
              <button className="btn btn-sm" onClick={() => setDiffModalOpen(false)}>
                Close
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Conflict Detection Modal ───────────────── */}
      {conflictModal?.open && (
        <div className="modal-backdrop" onClick={() => setConflictModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header" style={{ backgroundColor: "var(--danger-muted)" }}>
              <div style={{ fontWeight: 700, color: "var(--danger)", display: "flex", alignItems: "center", gap: 6 }}>
                <AlertTriangle size={16} />
                <span>External Modification Conflict</span>
              </div>
            </div>
            <div className="modal-body">
              <p style={{ fontSize: 13 }}>
                The file on disk has been modified externally since you loaded it into the editor.
              </p>
              <div style={{ fontSize: 11, fontFamily: "var(--font-mono)", color: "var(--text-muted)" }}>
                Current Disk Hash: {conflictModal.currentHash || "modified"}
              </div>
              <p style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                Would you like to overwrite the file on disk anyway, or review the changes in the External Changes inbox?
              </p>
            </div>
            <div className="modal-footer">
              <button
                className="btn btn-danger"
                onClick={() => handleSaveDocument(true)}
              >
                Force Overwrite
              </button>
              <button
                className="btn btn-primary"
                onClick={() => {
                  setConflictModal(null);
                  setActiveTab("external");
                  loadExternalChanges();
                }}
              >
                Review in External Inbox
              </button>
              <button className="btn" onClick={() => setConflictModal(null)}>
                Cancel
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Create File / Folder Modal ─────────────── */}
      {createModal && (
        <div className="modal-backdrop" onClick={() => setCreateModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13 }}>
                Create New {createModal === "file" ? "File" : "Directory"}
              </div>
              <button className="btn-icon" onClick={() => setCreateModal(null)} style={{ background: "transparent", border: "none" }}>
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 11.5, color: "var(--text-muted)" }}>Inside: {currentPath || "Root"}</div>
              <input
                type="text"
                autoFocus
                placeholder={createModal === "file" ? "e.g. document.txt" : "e.g. new_folder"}
                value={newEntityName}
                onChange={(e) => setNewEntityName(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") handleCreateEntity();
                  if (e.key === "Escape") setCreateModal(null);
                }}
                style={{
                  padding: "7px 10px",
                  borderRadius: "var(--radius-sm)",
                  border: "1px solid var(--border-default)",
                  fontSize: 13
                }}
              />
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setCreateModal(null)}>
                Cancel
              </button>
              <button className="btn btn-primary" onClick={handleCreateEntity}>
                Create
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Rename File / Folder Modal ─────────────── */}
      {renameTarget && (
        <div className="modal-backdrop" onClick={() => setRenameTarget(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 6 }}>
                <Edit3 size={15} color="var(--accent)" />
                <span>Rename {renameTarget.is_directory ? "Directory" : "File"}</span>
              </div>
              <button className="btn-icon" onClick={() => setRenameTarget(null)} style={{ background: "transparent", border: "none" }}>
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 11.5, color: "var(--text-muted)", marginBottom: 6 }}>
                Current Name: <strong style={{ color: "var(--text-primary)" }}>{renameTarget.name}</strong>
              </div>
              <input
                ref={renameInputRef}
                type="text"
                className="modal-input"
                placeholder="Enter new name"
                value={newRenameName}
                onChange={(e) => setNewRenameName(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") handleRenameSubmit();
                  if (e.key === "Escape") setRenameTarget(null);
                }}
                style={{
                  width: "100%",
                  padding: "8px 10px",
                  borderRadius: "var(--radius-sm)",
                  border: "1px solid var(--border-default)",
                  fontSize: 13,
                  backgroundColor: "var(--bg-surface)",
                  color: "var(--text-primary)"
                }}
              />
              <div style={{ fontSize: 11, color: "var(--text-secondary)", marginTop: 6, fontFamily: "var(--font-mono)", wordBreak: "break-all" }}>
                Location: {renameTarget.path}
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setRenameTarget(null)}>
                Cancel
              </button>
              <button
                className="btn btn-primary"
                onClick={handleRenameSubmit}
                disabled={!newRenameName.trim() || newRenameName.trim() === renameTarget.name}
              >
                Rename
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Add Managed Root Modal ─────────────────── */}
      {showAddRootModal && (
        <div className="modal-backdrop" onClick={() => setShowAddRootModal(false)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13 }}>Add Monitored Folder Root</div>
              <button className="btn-icon" onClick={() => setShowAddRootModal(false)} style={{ background: "transparent", border: "none" }}>
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 11.5, color: "var(--text-muted)" }}>
                Specify an absolute folder path on your drive to monitor and manage:
              </div>
              <input
                type="text"
                autoFocus
                placeholder="e.g. D:\Projects\MyFiles"
                value={newRootPath}
                onChange={(e) => setNewRootPath(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter") handleAddRoot();
                }}
                style={{
                  padding: "7px 10px",
                  borderRadius: "var(--radius-sm)",
                  border: "1px solid var(--border-default)",
                  fontSize: 13,
                  fontFamily: "var(--font-mono)"
                }}
              />
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setShowAddRootModal(false)}>
                Cancel
              </button>
              <button className="btn btn-primary" onClick={handleAddRoot}>
                Add Root
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Quarantine Confirmation Modal ─────────────── */}
      {quarantineModal && (
        <div className="modal-backdrop" onClick={() => setQuarantineModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 560 }}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 8 }}>
                <Archive size={16} color="var(--danger)" />
                <span>Confirm Quarantine Isolation</span>
              </div>
              <button
                className="btn-icon"
                onClick={() => setQuarantineModal(null)}
                style={{ background: "transparent", border: "none" }}
              >
                <X size={15} />
              </button>
            </div>

            <div className="modal-body">
              <div className="quarantine-callout">
                <strong>Safe Deletion & Recovery Guarantee:</strong>
                <div style={{ marginTop: 4, color: "var(--text-secondary)" }}>
                  Selected item{quarantineModal.items.length > 1 ? "s" : ""} will be unlinked from the active filesystem and moved into the isolated Quarantine Vault. File identity, revisions, and cryptographic hashes are preserved in SQLite WAL. You can restore item(s) to original or custom paths at any time.
                </div>
              </div>

              <div className="quarantine-meta-card">
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Target Items:</span>
                  <span className="quarantine-row-val">{quarantineModal.items.length} item(s)</span>
                </div>
                {quarantineModal.items.slice(0, 3).map((it) => (
                  <div key={it.path} className="quarantine-row">
                    <span className="quarantine-row-label" style={{ maxWidth: 220, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>
                      {it.name}:
                    </span>
                    <span className="quarantine-row-val" style={{ fontSize: 11 }}>
                      {formatBytes(it.size_bytes)} ({it.is_directory ? "Directory" : "File"})
                    </span>
                  </div>
                ))}
                {quarantineModal.items.length > 3 && (
                  <div style={{ fontSize: 11, color: "var(--text-muted)", fontStyle: "italic" }}>
                    + {quarantineModal.items.length - 3} more items...
                  </div>
                )}
                <div className="quarantine-row" style={{ marginTop: 4 }}>
                  <span className="quarantine-row-label">Managed Workspace:</span>
                  <span className="quarantine-row-val" style={{ color: "var(--success)" }}>Protected</span>
                </div>
              </div>

              <div>
                <label style={{ fontSize: 12, fontWeight: 600, display: "block", marginBottom: 4 }}>
                  Quarantine Reason / Audit Note:
                </label>
                <input
                  type="text"
                  className="modal-input"
                  value={quarantineModal.reason}
                  onChange={(e) => setQuarantineModal({ ...quarantineModal, reason: e.target.value })}
                  placeholder="e.g. Suspicious modification, user cleanup, test quarantine"
                />
              </div>
            </div>

            <div className="modal-footer">
              <button className="btn" onClick={() => setQuarantineModal(null)}>
                Cancel
              </button>
              <button className="btn btn-danger" onClick={handleQuarantineConfirm}>
                <Archive size={13} />
                <span>Isolate to Quarantine</span>
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Batch Move Modal ───────────────────────── */}
      {batchMoveModal && (
        <div className="modal-backdrop" onClick={() => setBatchMoveModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 520 }}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 8 }}>
                <Move size={16} color="var(--accent)" />
                <span>Batch Move Items</span>
              </div>
              <button
                className="btn-icon"
                onClick={() => setBatchMoveModal(null)}
                style={{ background: "transparent", border: "none" }}
              >
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                Moving <strong>{selectedPaths.size}</strong> selected item(s) inside a transactional batch.
              </div>
              <div>
                <label style={{ fontSize: 12, fontWeight: 600, display: "block", marginBottom: 4 }}>
                  Destination Directory Path:
                </label>
                <input
                  type="text"
                  className="modal-input"
                  value={batchMoveModal.targetDir}
                  onChange={(e) => setBatchMoveModal({ ...batchMoveModal, targetDir: e.target.value })}
                  placeholder="e.g. D:\EventDrivenFileSystem\watched\archive"
                />
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setBatchMoveModal(null)}>
                Cancel
              </button>
              <button className="btn btn-primary" onClick={handleBatchMoveConfirm}>
                Execute Batch Move
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Restore Custom Destination Modal ───────── */}
      {restoreCustomModal && (
        <div className="modal-backdrop" onClick={() => setRestoreCustomModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 540 }}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 8 }}>
                <RotateCcw size={16} color="var(--accent)" />
                <span>Restore to Custom Destination</span>
              </div>
              <button
                className="btn-icon"
                onClick={() => setRestoreCustomModal(null)}
                style={{ background: "transparent", border: "none" }}
              >
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 12, color: "var(--text-muted)" }}>
                Original Path: <span style={{ fontFamily: "var(--font-mono)", color: "var(--text-primary)" }}>{restoreCustomModal.item.original_path}</span>
              </div>
              <div>
                <label style={{ fontSize: 12, fontWeight: 600, display: "block", marginBottom: 4 }}>
                  Custom Destination Path:
                </label>
                <input
                  type="text"
                  className="modal-input"
                  value={restoreCustomModal.customDest}
                  onChange={(e) => setRestoreCustomModal({ ...restoreCustomModal, customDest: e.target.value })}
                  placeholder="e.g. D:\EventDrivenFileSystem\watched\restored_copy.txt"
                />
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setRestoreCustomModal(null)}>
                Cancel
              </button>
              <button
                className="btn btn-primary"
                onClick={() => handleRestoreQuarantine(restoreCustomModal.item, restoreCustomModal.customDest, false)}
                disabled={!restoreCustomModal.customDest.trim()}
              >
                Restore
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Restore Conflict Modal ─────────────────── */}
      {restoreConflictModal && (
        <div className="modal-backdrop" onClick={() => setRestoreConflictModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 540 }}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 8 }}>
                <AlertTriangle size={16} color="var(--warning)" />
                <span>Restore Conflict Detected</span>
              </div>
              <button
                className="btn-icon"
                onClick={() => setRestoreConflictModal(null)}
                style={{ background: "transparent", border: "none" }}
              >
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div className="diff-banner rename">
                {restoreConflictModal.errorMsg}
              </div>
              <div style={{ fontSize: 12, color: "var(--text-secondary)" }}>
                A file or folder already occupies destination: <br />
                <code style={{ fontFamily: "var(--font-mono)", fontSize: 11 }}>{restoreConflictModal.destination}</code>
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setRestoreConflictModal(null)}>
                Cancel
              </button>
              <button
                className="btn"
                onClick={() => {
                  const item = restoreConflictModal.item;
                  setRestoreConflictModal(null);
                  setRestoreCustomModal({
                    open: true,
                    item,
                    customDest: item.original_path + ".restored"
                  });
                }}
              >
                Choose Different Path
              </button>
              <button
                className="btn btn-danger"
                onClick={() => handleRestoreQuarantine(restoreConflictModal.item, restoreConflictModal.destination, true)}
              >
                Overwrite Existing
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Quarantine Artifact Details Modal ──────── */}
      {quarantineDetailModal && (
        <div className="modal-backdrop" onClick={() => setQuarantineDetailModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 560 }}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 8 }}>
                <Shield size={16} color="var(--accent)" />
                <span>Quarantine Artifact Details</span>
              </div>
              <button
                className="btn-icon"
                onClick={() => setQuarantineDetailModal(null)}
                style={{ background: "transparent", border: "none" }}
              >
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div className="quarantine-meta-card">
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Quarantine ID:</span>
                  <span className="quarantine-row-val">{quarantineDetailModal.id}</span>
                </div>
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Original Path:</span>
                  <span className="quarantine-row-val" style={{ wordBreak: "break-all" }}>{quarantineDetailModal.original_path}</span>
                </div>
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Vault Path:</span>
                  <span className="quarantine-row-val" style={{ wordBreak: "break-all" }}>{quarantineDetailModal.quarantine_path}</span>
                </div>
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Size:</span>
                  <span className="quarantine-row-val">{formatBytes(quarantineDetailModal.size_bytes)} ({quarantineDetailModal.size_bytes} bytes)</span>
                </div>
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Type:</span>
                  <span className="quarantine-row-val">{quarantineDetailModal.is_directory ? "Directory Tree" : "Regular File"}</span>
                </div>
                <div className="quarantine-row">
                  <span className="quarantine-row-label">Quarantined At:</span>
                  <span className="quarantine-row-val">{formatDate(quarantineDetailModal.quarantined_at)}</span>
                </div>
                {quarantineDetailModal.reason && (
                  <div className="quarantine-row">
                    <span className="quarantine-row-label">Reason:</span>
                    <span className="quarantine-row-val">{quarantineDetailModal.reason}</span>
                  </div>
                )}
                {quarantineDetailModal.sha256_hash && (
                  <div className="quarantine-row" style={{ flexDirection: "column", alignItems: "flex-start", gap: 2 }}>
                    <span className="quarantine-row-label">SHA-256 Checksum:</span>
                    <span className="quarantine-row-val" style={{ fontSize: 10, wordBreak: "break-all" }}>
                      {quarantineDetailModal.sha256_hash}
                    </span>
                  </div>
                )}
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setQuarantineDetailModal(null)}>
                Close
              </button>
            </div>
          </div>
        </div>
      )}

      {/* ── Purge Confirmation Modal ───────────────── */}
      {purgeConfirmModal && (
        <div className="modal-backdrop" onClick={() => setPurgeConfirmModal(null)}>
          <div className="modal-dialog" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 480 }}>
            <div className="modal-header">
              <div style={{ fontWeight: 700, fontSize: 13, display: "flex", alignItems: "center", gap: 8, color: "var(--danger)" }}>
                <AlertTriangle size={16} />
                <span>Permanently Purge Quarantined Item?</span>
              </div>
              <button
                className="btn-icon"
                onClick={() => setPurgeConfirmModal(null)}
                style={{ background: "transparent", border: "none" }}
              >
                <X size={15} />
              </button>
            </div>
            <div className="modal-body">
              <div style={{ fontSize: 12.5, lineHeight: 1.5 }}>
                You are about to permanently delete <strong style={{ fontFamily: "var(--font-mono)" }}>{pathBasename(purgeConfirmModal.original_path)}</strong> from the quarantine vault.
              </div>
              <div className="quarantine-callout" style={{ borderLeftColor: "var(--danger)", backgroundColor: "var(--danger-muted)", color: "var(--danger)" }}>
                <strong>Warning: This action is permanent and unrecoverable.</strong> The file payload will be purged from physical storage.
              </div>
            </div>
            <div className="modal-footer">
              <button className="btn" onClick={() => setPurgeConfirmModal(null)}>
                Cancel
              </button>
              <button className="btn btn-danger" onClick={() => handlePurgeQuarantine(purgeConfirmModal)}>
                Permanently Delete
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
export default App;
