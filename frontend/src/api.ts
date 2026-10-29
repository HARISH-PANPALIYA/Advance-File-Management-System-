// Centralized typed FileGuard API client

export interface FileItem {
  path: string;
  name: string;
  extension: string;
  is_directory: boolean;
  size_bytes: number;
  modified_at: number; // ms unix timestamp
  is_readonly: boolean;
  hash?: string;
}

export interface DirectoryListing {
  current_path: string;
  parent_path: string;
  items: FileItem[];
  total_files: number;
  total_directories: number;
  total_size_bytes: number;
}

export interface OperationResult {
  success: boolean;
  operation_id?: string;
  message?: string;
  error?: string;
  has_conflict?: boolean;
  conflict_reason?: string;
  can_undo?: boolean;
  can_redo?: boolean;
}

export interface HealthStatus {
  status: string;
  app: string;
  version: string;
  can_undo: boolean;
  can_redo: boolean;
  can_undo_content?: boolean;
  can_redo_content?: boolean;
  can_undo_name?: boolean;
  can_redo_name?: boolean;
  last_undo_op?: { id: string; type: string; source: string; dest: string; transaction_id?: string };
  last_redo_op?: { id: string; type: string; source: string; dest: string; transaction_id?: string };
  last_undo_content_op?: { id: string; type: string; source: string; dest: string; transaction_id?: string };
  last_redo_content_op?: { id: string; type: string; source: string; dest: string; transaction_id?: string };
  last_undo_name_op?: { id: string; type: string; source: string; dest: string; transaction_id?: string };
  last_redo_name_op?: { id: string; type: string; source: string; dest: string; transaction_id?: string };
}

export interface OperationHistoryItem {
  id: string;
  seq: number;
  type: string;
  status: string;
  source_path: string;
  dest_path: string;
  transaction_id: string;
  created_at: string;
  completed_at: string;
  is_undoable: boolean;
  is_undone: boolean;
  error_message?: string;
}

export interface FilesystemEvent {
  id: number;
  kind: string;
  path: string;
  old_path?: string;
  timestamp: string;
  pid: number;
  is_external: boolean;
}

export interface QuarantineItem {
  id: string;
  original_path: string;
  quarantine_path: string;
  quarantined_at: string;
  size_bytes: number;
  sha256_hash: string;
  is_directory: boolean;
  reason?: string;
}

export interface VersionItem {
  id: number;
  path: string;
  created_at: string;
  size_bytes: number;
  sha256_hash: string;
  source_op_id: string;
}

export interface WatcherStatus {
  is_active: boolean;
  watched_path: string;
  backend_type: string;
  events_captured: number;
  overflow_count: number;
  last_event_time: string;
}

export interface StorageStats {
  quarantine_bytes: number;
  snapshots_bytes: number;
  total_bytes: number;
  quarantine_items: number;
  snapshots_count: number;
}

export interface SettingsData {
  approved_roots: string[];
  watched_path: string;
  watcher_active: boolean;
  quarantine_dir: string;
  snapshots_dir: string;
  storage_usage: StorageStats;
}

export interface BenchmarkRecord {
  id: string;
  type: string;
  timestamp: string;
  duration_ms: number;
  items_processed: number;
  throughput: number;
  details: Record<string, unknown>;
}

export interface FileContentResponse {
  path: string;
  size_bytes: number;
  is_binary: boolean;
  content: string;
}

export interface DriveInfo {
  letter: string;
  label: string;
  path: string;
  total_bytes: number;
  free_bytes: number;
}

export interface ManagedRoot {
  path: string;
  name: string;
  is_monitored: boolean;
  is_demo: boolean;
  added_at: string;
}

export interface FileIdentity {
  id: string;
  current_path: string;
  initial_path: string;
  created_at: string;
  last_modified_at: string;
}

export interface NameLocationRecord {
  id: string;
  op_type: string;
  source_path: string;
  dest_path: string;
  created_at: string;
}

export interface ContentRevision {
  id: string;
  revision_number: number;
  path: string;
  size_bytes: number;
  sha256_hash: string;
  snapshot_id: number;
  source_type: string;
  change_summary: string;
  created_at: string;
}

export interface FileHistoryResponse {
  file_identity: FileIdentity | null;
  name_location_history: NameLocationRecord[];
  content_revisions: ContentRevision[];
  operations: OperationHistoryItem[];
  external_candidates?: ExternalCandidate[];
}

export interface ExternalCandidate {
  id: string;
  seq: number;
  path: string;
  old_path?: string;
  file_identity_id?: string;
  change_type: string;
  detected_at: string;
  status: "PENDING" | "ACCEPTED" | "REJECTED" | "RECOVERED";
  candidate_snapshot_path: string;
  candidate_hash: string;
  candidate_size: number;
  prior_accepted_hash: string;
  prior_accepted_snapshot_id: number;
  resolution_notes?: string;
  resolved_at?: string;
}

export interface ExternalDiffResponse {
  candidate_id: string;
  path: string;
  prior_content: string;
  candidate_content: string;
  candidate_hash: string;
  prior_hash: string;
}

export interface FileTypeStat {
  extension: string;
  count: number;
  total_bytes: number;
}

export interface DuplicateGroup {
  sha256_hash: string;
  size_bytes: number;
  paths: string[];
  reclaimable_bytes: number;
}

export interface LargestFileInfo {
  path: string;
  name: string;
  size_bytes: number;
}

export interface FolderInsights {
  folder_path: string;
  total_files: number;
  total_directories: number;
  total_bytes: number;
  reclaimable_bytes: number;
  type_distribution: FileTypeStat[];
  duplicates: DuplicateGroup[];
  largest_files: LargestFileInfo[];
  empty_directories: string[];
}

export interface TransactionItem {
  id: string;
  type: string;
  status: string;
  total_ops: number;
  completed_ops: number;
  failed_ops: number;
  created_at: string;
  completed_at?: string;
  error_message?: string;
  operations: OperationHistoryItem[];
}

export interface DocumentSaveResult {
  success: boolean;
  conflict?: boolean;
  error?: string;
  new_hash?: string;
  revision_id?: string;
  revision_number?: number;
  file_identity_id?: string;
  current_hash?: string;
  expected_hash?: string;
}

// Path Helper Utilities for Cross-Platform Desktop Management
export function normalizePath(p: string): string {
  if (!p) return "";
  let norm = p.replace(/\\/g, "/");
  while (norm.length > 1 && norm.endsWith("/")) {
    norm = norm.slice(0, -1);
  }
  return norm;
}

export function pathBasename(p: string): string {
  const norm = normalizePath(p);
  const parts = norm.split("/").filter(Boolean);
  return parts.length > 0 ? parts[parts.length - 1] : norm;
}

export function pathDirname(p: string): string {
  const norm = normalizePath(p);
  const idx = norm.lastIndexOf("/");
  if (idx <= 0) return "";
  return norm.substring(0, idx);
}

export function pathJoin(...parts: string[]): string {
  return parts
    .map((part) => normalizePath(part))
    .filter(Boolean)
    .join("/")
    .replace(/\/+/g, "/");
}

export class ApiError extends Error {
  status: number;
  data: unknown;
  hasConflict?: boolean;
  conflictReason?: string;

  constructor(message: string, status: number, data?: unknown) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.data = data;
    if (data && typeof data === "object") {
      const d = data as Record<string, unknown>;
      if (d.has_conflict) this.hasConflict = Boolean(d.has_conflict);
      if (d.conflict_reason) this.conflictReason = String(d.conflict_reason);
    }
  }
}

// Configurable API base; default to Vite proxy /api
const API_BASE = ((import.meta as any).env?.VITE_API_URL as string) || "/api";

async function request<T>(endpoint: string, options?: RequestInit): Promise<T> {
  const url = `${API_BASE}${endpoint}`;
  const controller = new AbortController();
  const timeoutId = setTimeout(() => controller.abort(), 12000); // 12s timeout

  try {
    const res = await fetch(url, {
      ...options,
      signal: controller.signal,
      headers: {
        "Content-Type": "application/json",
        ...(options?.headers || {}),
      },
    });

    clearTimeout(timeoutId);

    const data = await res.json().catch(() => ({ error: `Failed to parse JSON response (${res.status})` }));
    if (!res.ok) {
      const errMessage = data.error || data.message || `Request failed with status ${res.status}`;
      throw new ApiError(errMessage, res.status, data);
    }
    return data as T;
  } catch (err: unknown) {
    clearTimeout(timeoutId);
    if (err instanceof ApiError) throw err;
    if (err instanceof Error) {
      if (err.name === "AbortError") {
        throw new ApiError("Request timed out: C++ backend took longer than 12 seconds to respond.", 504);
      }
      throw new ApiError(err.message, 0);
    }
    throw new ApiError(String(err), 0);
  }
}

export const api = {
  getHealth: () => request<HealthStatus>("/health"),

  getFiles: (path?: string) =>
    request<DirectoryListing>(`/files${path ? `?path=${encodeURIComponent(path)}` : ""}`),

  getMetadata: (path: string) =>
    request<FileItem>(`/metadata?path=${encodeURIComponent(path)}`),

  getFileContent: (path: string) =>
    request<FileContentResponse>(`/file/content?path=${encodeURIComponent(path)}`),

  getDownloadUrl: (path: string) =>
    `${API_BASE}/file/download?path=${encodeURIComponent(path)}`,

  getBenchmarkCsvUrl: () =>
    `${API_BASE}/benchmarks?format=csv`,

  getEventStreamUrl: () =>
    `${API_BASE}/events/stream`,

  search: (query: string, root?: string, ext?: string, minSize?: number, maxSize?: number, content?: string) => {
    const params = new URLSearchParams({ query });
    if (root) params.set("root", root);
    if (ext) params.set("ext", ext);
    if (minSize !== undefined) params.set("min_size", String(minSize));
    if (maxSize !== undefined) params.set("max_size", String(maxSize));
    if (content) params.set("content", content);
    return request<FileItem[]>(`/search?${params.toString()}`);
  },

  createFile: (path: string, content = "") =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "CREATE_FILE", path, content }),
    }),

  createDirectory: (path: string) =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "CREATE_DIR", path }),
    }),

  renameItem: (source: string, destination: string) =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "RENAME", source, destination }),
    }),

  moveItem: (source: string, destination: string) =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "MOVE", source, destination }),
    }),

  copyItem: (source: string, destination: string, overwrite = false) =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "COPY", source, destination, overwrite }),
    }),

  deleteItem: (path: string) =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "DELETE", path }),
    }),

  overwriteFile: (path: string, content: string) =>
    request<OperationResult>("/operations", {
      method: "POST",
      body: JSON.stringify({ type: "OVERWRITE", path, content }),
    }),

  undo: (scope?: "content" | "name" | "all") =>
    request<OperationResult>(`/undo${scope ? `?scope=${scope}` : ""}`, {
      method: "POST",
      body: JSON.stringify({ scope: scope || "all" }),
    }),

  redo: (scope?: "content" | "name" | "all") =>
    request<OperationResult>(`/redo${scope ? `?scope=${scope}` : ""}`, {
      method: "POST",
      body: JSON.stringify({ scope: scope || "all" }),
    }),

  undoContent: () =>
    request<OperationResult>("/undo?scope=content", {
      method: "POST",
      body: JSON.stringify({ scope: "content" }),
    }),

  redoContent: () =>
    request<OperationResult>("/redo?scope=content", {
      method: "POST",
      body: JSON.stringify({ scope: "content" }),
    }),

  undoName: () =>
    request<OperationResult>("/undo?scope=name", {
      method: "POST",
      body: JSON.stringify({ scope: "name" }),
    }),

  redoName: () =>
    request<OperationResult>("/redo?scope=name", {
      method: "POST",
      body: JSON.stringify({ scope: "name" }),
    }),

  getHistory: (limit = 100) =>
    request<OperationHistoryItem[]>(`/history?limit=${limit}`),

  getVersions: (path: string) =>
    request<VersionItem[]>(`/versions?path=${encodeURIComponent(path)}`),

  createCheckpoint: (path: string) =>
    request<{ success: boolean; id: number; path: string }>("/versions/checkpoint", {
      method: "POST",
      body: JSON.stringify({ path }),
    }),

  restoreVersion: (version_id: number, backup_current = true) =>
    request<{ success: boolean; message: string }>("/restore", {
      method: "POST",
      body: JSON.stringify({ version_id, backup_current }),
    }),

  getQuarantine: () => request<QuarantineItem[]>("/quarantine"),

  quarantineItem: (path: string, reason = "User initiated quarantine") =>
    request<OperationResult>("/quarantine", {
      method: "POST",
      body: JSON.stringify({ path, reason }),
    }),

  restoreQuarantine: (id: string, destination?: string, overwrite = false) =>
    request<{ success: boolean; message: string }>("/quarantine/restore", {
      method: "POST",
      body: JSON.stringify({ id, destination: destination || "", overwrite }),
    }),

  deleteQuarantine: (id: string) =>
    request<{ success: boolean; message: string }>("/quarantine/delete", {
      method: "POST",
      body: JSON.stringify({ id }),
    }),

  batchDelete: (paths: string[]) =>
    request<{ success: boolean; transaction_id: string; message: string }>("/transactions", {
      method: "POST",
      body: JSON.stringify({ type: "BATCH_DELETE", paths }),
    }),

  batchMove: (items: Array<{ source: string; destination: string }>) =>
    request<{ success: boolean; transaction_id: string; message: string }>("/transactions", {
      method: "POST",
      body: JSON.stringify({ type: "BATCH_MOVE", items }),
    }),

  batchCopy: (items: Array<{ source: string; destination: string }>) =>
    request<{ success: boolean; transaction_id: string; message: string }>("/transactions", {
      method: "POST",
      body: JSON.stringify({ type: "BATCH_COPY", items }),
    }),

  batchRename: (items: Array<{ source: string; destination: string }>) =>
    request<{ success: boolean; transaction_id: string; message: string }>("/transactions", {
      method: "POST",
      body: JSON.stringify({ type: "BATCH_RENAME", items }),
    }),

  getWatchStatus: () => request<WatcherStatus>("/watch/status"),

  startWatch: (path: string) =>
    request<{ success: boolean; watched_path: string }>("/watch/start", {
      method: "POST",
      body: JSON.stringify({ path }),
    }),

  stopWatch: () =>
    request<{ success: boolean; message: string }>("/watch/stop", {
      method: "POST",
    }),

  getEvents: (limit = 100) =>
    request<FilesystemEvent[]>(`/events?limit=${limit}`),

  runBenchmark: (experiment: "scan" | "watcher" | "journal", params?: Record<string, unknown>) =>
    request<Record<string, unknown>>("/benchmarks/run", {
      method: "POST",
      body: JSON.stringify({ experiment, ...(params || {}) }),
    }),

  getBenchmarks: () => request<BenchmarkRecord[]>("/benchmarks"),

  getSettings: () => request<SettingsData>("/settings"),

  updateSettings: (settings: Partial<SettingsData>) =>
    request<{ success: boolean }>("/settings", {
      method: "POST",
      body: JSON.stringify(settings),
    }),

  // Phase 2 Endpoints
  getSystemDrives: () => request<DriveInfo[]>("/system/drives"),

  getRoots: () => request<ManagedRoot[]>("/roots"),

  addRoot: (path: string, name?: string, is_monitored = true, is_demo = false) =>
    request<{ success: boolean; root: ManagedRoot }>("/roots/add", {
      method: "POST",
      body: JSON.stringify({ path, name, is_monitored, is_demo }),
    }),

  removeRoot: (path: string) =>
    request<{ success: boolean }>("/roots/remove", {
      method: "POST",
      body: JSON.stringify({ path }),
    }),

  populateDemoWorkspace: (path?: string) =>
    request<{ success: boolean; path: string }>("/demo/populate", {
      method: "POST",
      body: JSON.stringify(path ? { path } : {}),
    }),

  saveDocument: (path: string, content: string, expected_pre_hash = "", change_summary = "Document edit in FileGuard") =>
    request<DocumentSaveResult>("/file/save", {
      method: "POST",
      body: JSON.stringify({ path, content, expected_pre_hash, change_summary }),
    }),

  getFileHistory: (path: string) =>
    request<FileHistoryResponse>(`/history/file?path=${encodeURIComponent(path)}`),

  getFolderHistory: (path: string) =>
    request<{ folder_path: string; operations: OperationHistoryItem[] }>(`/history/folder?path=${encodeURIComponent(path)}`),

  getRevisions: (path: string) =>
    request<ContentRevision[]>(`/revisions?path=${encodeURIComponent(path)}`),

  restoreRevision: (snapshot_id: number, path: string) =>
    request<{ success: boolean; revision_id: string; new_hash: string }>("/revisions/restore", {
      method: "POST",
      body: JSON.stringify({ snapshot_id, path }),
    }),

  getExternalChanges: (status?: string) =>
    request<ExternalCandidate[]>(`/external-changes${status ? `?status=${encodeURIComponent(status)}` : ""}`),

  acceptExternalChange: (id: string, notes = "Accepted external modification") =>
    request<{ success: boolean; candidate_id: string; revision_id: string }>("/external-changes/accept", {
      method: "POST",
      body: JSON.stringify({ id, notes }),
    }),

  rejectExternalChange: (id: string, notes = "Rejected external modification") =>
    request<{ success: boolean; candidate_id: string; status: string }>("/external-changes/reject", {
      method: "POST",
      body: JSON.stringify({ id, notes }),
    }),

  recoverExternalChange: (id: string, mode: "new_copy" | "new_revision" = "new_copy", target_path = "") =>
    request<{ success: boolean; recovered_path?: string; revision_id?: string; revision_number?: number }>("/external-changes/recover", {
      method: "POST",
      body: JSON.stringify({ id, mode, target_path }),
    }),

  getExternalChangeDiff: (id: string) =>
    request<ExternalDiffResponse>(`/external-changes/diff?id=${encodeURIComponent(id)}`),

  getFolderInsights: (path?: string) =>
    request<FolderInsights>(`/insights${path ? `?path=${encodeURIComponent(path)}` : ""}`),

  getTransactions: () => request<TransactionItem[]>("/transactions"),

  getTransactionDetails: (id: string) =>
    request<TransactionItem>(`/transactions/${encodeURIComponent(id)}`),

  undoTransaction: (transaction_id: string) =>
    request<OperationResult>("/transactions/undo", {
      method: "POST",
      body: JSON.stringify({ transaction_id, id: transaction_id }),
    }),

  redoTransaction: (transaction_id: string) =>
    request<OperationResult>("/transactions/redo", {
      method: "POST",
      body: JSON.stringify({ transaction_id, id: transaction_id }),
    }),
};
