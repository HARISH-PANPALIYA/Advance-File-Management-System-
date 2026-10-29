#include "journal.hpp"
#include "sha256.hpp"
#include "filesystem_service.hpp"
#include "recovery_service.hpp"
#include "command_manager.hpp"
#include "benchmark_service.hpp"
#include "fileguard_tests.hpp"
#include "json.hpp"

#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fileguard {
using json = nlohmann::json;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] Line " << __LINE__ << ": " << msg << "\n"; \
            return false; \
        } \
    } while (0)

#define RUN_TEST(fn) \
    do { \
        std::cout << "[RUNNING] " << #fn << " ... "; \
        if (fn()) { \
            std::cout << "[PASSED]\n"; \
            passed++; \
        } else { \
            std::cout << "[FAILED]\n"; \
            failed++; \
        } \
    } while (0)

static std::filesystem::path test_sandbox;

void setup_sandbox() {
    test_sandbox = std::filesystem::current_path() / "test_scratch_sandbox";
    std::error_code ec;
    std::filesystem::remove_all(test_sandbox, ec);
    std::filesystem::create_directories(test_sandbox, ec);
    std::filesystem::create_directories(test_sandbox / "storage" / "snapshots", ec);
    std::filesystem::create_directories(test_sandbox / "storage" / "quarantine", ec);
}

void cleanup_sandbox() {
    std::error_code ec;
    std::filesystem::remove_all(test_sandbox, ec);
}

// ── Test 1: SHA-256 Hashing ──────────────────────────────────────────
bool test_sha256() {
    std::string empty_hash = sha256_string("");
    TEST_ASSERT(empty_hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "Empty string SHA-256");

    std::string hello_hash = sha256_string("hello world");
    TEST_ASSERT(hello_hash == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9", "hello world SHA-256");

    auto p = test_sandbox / "sample.bin";
    std::vector<uint8_t> binary_data = {0x00, 0xFF, 0x12, 0x34, 0xAB, 0xCD};
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(binary_data.data()), binary_data.size());
    out.close();

    std::string file_h = sha256_file(p);
    std::string buf_h = sha256_buffer(binary_data);
    TEST_ASSERT(file_h == buf_h, "File SHA-256 matches buffer SHA-256");
    return true;
}

// ── Test 2: Filesystem Operations & Canonical Path Safety ───────────
bool test_filesystem_operations() {
    std::vector<std::filesystem::path> roots = { test_sandbox };
    FilesystemService fs(roots);

    // Approved root restriction
    TEST_ASSERT(fs.is_path_approved(test_sandbox / "file.txt"), "Path inside approved root");
    TEST_ASSERT(!fs.is_path_approved(test_sandbox / ".." / "outside_dir"), "Path traversal outside approved root rejected");

    // Create file
    auto f1 = test_sandbox / "doc.txt";
    TEST_ASSERT(fs.create_file(f1, "Initial text"), "Create file");
    TEST_ASSERT(std::filesystem::exists(f1), "File exists on disk");

    // Metadata
    auto meta = fs.get_metadata(f1, true);
    TEST_ASSERT(meta.name == "doc.txt", "Metadata name");
    TEST_ASSERT(meta.size_bytes == 12, "Metadata size");
    TEST_ASSERT(meta.extension == "txt", "Metadata extension");
    TEST_ASSERT(!meta.hash.empty(), "Metadata hash computed");

    // Unicode & space filename test
    auto f_unicode = test_sandbox / "résumé dossier 2026.txt";
    TEST_ASSERT(fs.create_file(f_unicode, "Unicode content"), "Create unicode path with spaces");
    TEST_ASSERT(std::filesystem::exists(f_unicode), "Unicode file exists");

    // Create directory
    auto d1 = test_sandbox / "nested" / "folder";
    TEST_ASSERT(fs.create_directory(d1), "Create directory");
    TEST_ASSERT(std::filesystem::is_directory(d1), "Dir exists on disk");

    // Rename
    auto f2 = test_sandbox / "renamed_doc.txt";
    TEST_ASSERT(fs.rename_item(f1, f2), "Rename item");
    TEST_ASSERT(!std::filesystem::exists(f1), "Old path no longer exists");
    TEST_ASSERT(std::filesystem::exists(f2), "New path exists");

    // Move
    auto f_moved = d1 / "moved_doc.txt";
    TEST_ASSERT(fs.move_item(f2, f_moved), "Move item");
    TEST_ASSERT(!std::filesystem::exists(f2), "Moved from old path");
    TEST_ASSERT(std::filesystem::exists(f_moved), "Moved to new path");

    // Copy
    auto f_copy = test_sandbox / "copied_doc.txt";
    TEST_ASSERT(fs.copy_item(f_moved, f_copy), "Copy item");
    TEST_ASSERT(std::filesystem::exists(f_moved), "Source still exists after copy");
    TEST_ASSERT(std::filesystem::exists(f_copy), "Copy destination exists");

    // Search
    SearchOptions sopt;
    sopt.root_path = test_sandbox.string();
    sopt.query = "résumé";
    auto srch = fs.search(sopt);
    TEST_ASSERT(srch.size() >= 1, "Search finds unicode file");

    return true;
}

// ── Test 3: Single Undo & Redo (Create, Rename, Move, Copy, Delete, Overwrite)
bool test_undo_redo_primitives() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    auto storage = test_sandbox / "storage";

    Journal journal(db);
    FilesystemService fs({ test_sandbox });
    RecoveryService recovery(journal, fs, storage);
    CommandManager mgr(journal, fs, recovery);

    // 1. Create file & Undo & Redo
    auto tf = test_sandbox / "undo_test.txt";
    auto cr1 = mgr.execute_create_file(tf, "Hello FileGuard");
    TEST_ASSERT(cr1.success, "execute_create_file succeeds");
    TEST_ASSERT(std::filesystem::exists(tf), "File exists after create");
    TEST_ASSERT(mgr.can_undo(), "can_undo is true");

    auto u1 = mgr.undo_last();
    TEST_ASSERT(u1.success, "undo_create succeeds");
    TEST_ASSERT(!std::filesystem::exists(tf), "File removed after undo create");
    TEST_ASSERT(mgr.can_redo(), "can_redo is true");

    auto r1 = mgr.redo_last();
    TEST_ASSERT(r1.success, "redo_create succeeds");
    TEST_ASSERT(std::filesystem::exists(tf), "File restored after redo create");

    // 2. Rename & Undo & Redo
    auto tf_renamed = test_sandbox / "undo_test_renamed.txt";
    auto cr2 = mgr.execute_rename(tf, tf_renamed);
    TEST_ASSERT(cr2.success, "execute_rename succeeds");
    TEST_ASSERT(std::filesystem::exists(tf_renamed), "Renamed file exists");

    auto u2 = mgr.undo_last();
    TEST_ASSERT(u2.success, "undo_rename succeeds");
    TEST_ASSERT(std::filesystem::exists(tf), "Reverted back to original name");
    TEST_ASSERT(!std::filesystem::exists(tf_renamed), "Renamed target gone");

    auto r2 = mgr.redo_last();
    TEST_ASSERT(r2.success, "redo_rename succeeds");
    TEST_ASSERT(std::filesystem::exists(tf_renamed), "Redo renamed file exists");

    // 3. Move & Undo & Redo
    auto target_dir = test_sandbox / "target_dir";
    fs.create_directory(target_dir);
    auto tf_moved = target_dir / "undo_test_renamed.txt";

    auto cr3 = mgr.execute_move(tf_renamed, tf_moved);
    TEST_ASSERT(cr3.success, "execute_move succeeds");
    TEST_ASSERT(std::filesystem::exists(tf_moved), "Moved file exists");

    auto u3 = mgr.undo_last();
    TEST_ASSERT(u3.success, "undo_move succeeds");
    TEST_ASSERT(std::filesystem::exists(tf_renamed), "Returned to pre-move location");

    auto r3 = mgr.redo_last();
    TEST_ASSERT(r3.success, "redo_move succeeds");
    TEST_ASSERT(std::filesystem::exists(tf_moved), "Redo moved file exists");

    // 4. Overwrite & Undo & Redo
    std::string new_data = "Overwritten content with version history!";
    std::vector<uint8_t> new_bytes(new_data.begin(), new_data.end());
    auto cr4 = mgr.execute_overwrite(tf_moved, new_bytes);
    TEST_ASSERT(cr4.success, "execute_overwrite succeeds");

    std::vector<uint8_t> read_back;
    fs.read_file(tf_moved, read_back);
    std::string read_str(read_back.begin(), read_back.end());
    TEST_ASSERT(read_str == new_data, "Content is overwritten");

    auto u4 = mgr.undo_last();
    TEST_ASSERT(u4.success, "undo_overwrite succeeds");
    fs.read_file(tf_moved, read_back);
    std::string pre_str(read_back.begin(), read_back.end());
    TEST_ASSERT(pre_str == "Hello FileGuard", "Undo overwrite restored pre-image");

    auto r4 = mgr.redo_last();
    TEST_ASSERT(r4.success, "redo_overwrite succeeds");
    fs.read_file(tf_moved, read_back);
    std::string post_str(read_back.begin(), read_back.end());
    TEST_ASSERT(post_str == new_data, "Redo overwrite restored post-image");

    // 5. Delete (Quarantine) & Undo & Redo
    auto cr5 = mgr.execute_delete(tf_moved);
    TEST_ASSERT(cr5.success, "execute_delete succeeds");
    TEST_ASSERT(!std::filesystem::exists(tf_moved), "Deleted file moved to quarantine");
    auto qlist = recovery.list_quarantine();
    TEST_ASSERT(!qlist.empty(), "Item appears in quarantine");

    auto u5 = mgr.undo_last();
    TEST_ASSERT(u5.success, "undo_delete succeeds (restored from quarantine)");
    TEST_ASSERT(std::filesystem::exists(tf_moved), "File restored to original path");

    auto r5 = mgr.redo_last();
    TEST_ASSERT(r5.success, "redo_delete succeeds (re-quarantined)");
    TEST_ASSERT(!std::filesystem::exists(tf_moved), "File removed again on redo delete");

    return true;
}

// ── Test 4: Persistent Undo/Redo Across Application Restarts ────────
bool test_persistent_restart() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    auto storage = test_sandbox / "storage";

    auto test_file = test_sandbox / "restart_test.txt";

    // Session 1: Create file, then close everything
    {
        Journal j1(db);
        FilesystemService fs1({ test_sandbox });
        RecoveryService r1(j1, fs1, storage);
        CommandManager mgr1(j1, fs1, r1);

        auto res = mgr1.execute_create_file(test_file, "Persistent state test");
        TEST_ASSERT(res.success, "Session 1 create file");
    }

    // Session 2: New application instance, verify state and execute undo
    {
        Journal j2(db);
        FilesystemService fs2({ test_sandbox });
        RecoveryService r2(j2, fs2, storage);
        CommandManager mgr2(j2, fs2, r2);

        TEST_ASSERT(mgr2.can_undo(), "Session 2 can_undo after restart");
        auto u = mgr2.undo_last();
        TEST_ASSERT(u.success, "Session 2 successfully undid Session 1 operation");
        TEST_ASSERT(!std::filesystem::exists(test_file), "File removed by undo across restart");
    }

    // Session 3: New application instance, verify redo works across restart
    {
        Journal j3(db);
        FilesystemService fs3({ test_sandbox });
        RecoveryService r3(j3, fs3, storage);
        CommandManager mgr3(j3, fs3, r3);

        TEST_ASSERT(mgr3.can_redo(), "Session 3 can_redo after restart");
        auto r = mgr3.redo_last();
        TEST_ASSERT(r.success, "Session 3 successfully redid operation across restart");
        TEST_ASSERT(std::filesystem::exists(test_file), "File restored by redo across restart");
    }

    return true;
}

// ── Test 5: Redo Branch Invalidation ─────────────────────────────────
bool test_redo_branch_invalidation() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    auto storage = test_sandbox / "storage";

    Journal j(db);
    FilesystemService fs({ test_sandbox });
    RecoveryService r(j, fs, storage);
    CommandManager mgr(j, fs, r);

    auto f_a = test_sandbox / "branch_a.txt";
    auto f_b = test_sandbox / "branch_b.txt";

    mgr.execute_create_file(f_a, "A");
    TEST_ASSERT(mgr.can_undo(), "can undo after A");

    // Undo A
    mgr.undo_last();
    TEST_ASSERT(mgr.can_redo(), "can redo A");

    // Perform a new mutation (B)
    mgr.execute_create_file(f_b, "B");

    // Old redo branch (A) must now be invalidated!
    TEST_ASSERT(!mgr.can_redo(), "Redo branch correctly invalidated after new mutation");
    return true;
}

// ── Test 6: Conflict Detection on External Modification ──────────────
bool test_conflict_detection() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    auto storage = test_sandbox / "storage";

    Journal j(db);
    FilesystemService fs({ test_sandbox });
    RecoveryService r(j, fs, storage);
    CommandManager mgr(j, fs, r);

    auto f_conflict = test_sandbox / "conflict_target.txt";
    mgr.execute_create_file(f_conflict, "Original fileguard content");

    // Simulate external process modifying the file behind FileGuard's back
    {
        std::ofstream ext(f_conflict, std::ios::binary | std::ios::trunc);
        ext << "Tampered by external editor!";
        ext.close();
    }

    // Attempting to undo creation must detect the content conflict and NOT silently delete!
    auto u_res = mgr.undo_last();
    TEST_ASSERT(!u_res.success, "Undo rejected due to conflict");
    TEST_ASSERT(u_res.has_conflict, "has_conflict is true");
    TEST_ASSERT(std::filesystem::exists(f_conflict), "File protected: not silently deleted!");
    return true;
}

// ── Test 7: Batch Transaction Rollback & Compensation ────────────────
bool test_batch_transactions() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    auto storage = test_sandbox / "storage";

    Journal j(db);
    FilesystemService fs({ test_sandbox });
    RecoveryService r(j, fs, storage);
    CommandManager mgr(j, fs, r);

    // 1. Successful batch delete
    auto f1 = test_sandbox / "batch_1.txt";
    auto f2 = test_sandbox / "batch_2.txt";
    auto f3 = test_sandbox / "batch_3.txt";

    fs.create_file(f1, "1");
    fs.create_file(f2, "2");
    fs.create_file(f3, "3");

    std::vector<std::string> paths = { f1.string(), f2.string(), f3.string() };
    auto res_batch = mgr.execute_batch_delete(paths);
    TEST_ASSERT(res_batch.success, "Successful batch delete");
    TEST_ASSERT(!std::filesystem::exists(f1), "Batch 1 deleted");
    TEST_ASSERT(!std::filesystem::exists(f2), "Batch 2 deleted");
    TEST_ASSERT(!std::filesystem::exists(f3), "Batch 3 deleted");

    // 2. Batch failure with automatic compensation / rollback
    // Create files A, B, but file C does NOT exist
    auto fA = test_sandbox / "batch_A.txt";
    auto fB = test_sandbox / "batch_B.txt";
    auto fC = test_sandbox / "batch_nonexistent.txt";

    fs.create_file(fA, "A");
    fs.create_file(fB, "B");

    std::vector<std::string> failing_paths = { fA.string(), fB.string(), fC.string() };
    auto res_fail = mgr.execute_batch_delete(failing_paths);
    TEST_ASSERT(!res_fail.success, "Batch with missing item fails");

    // Compensation: fA and fB should have been rolled back from quarantine!
    TEST_ASSERT(std::filesystem::exists(fA), "Item A compensated and restored");
    TEST_ASSERT(std::filesystem::exists(fB), "Item B compensated and restored");

    return true;
}

// ── Test 8: Benchmark Engine Reproducibility ─────────────────────────
bool test_benchmarks() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    auto storage = test_sandbox / "storage";

    Journal j(db);
    FilesystemService fs({ test_sandbox });
    BenchmarkService bench(j, fs, test_sandbox / "bench_scratch");

    // Scan experiment
    auto r_scan = bench.run_scan_benchmark(50, 2);
    TEST_ASSERT(r_scan.correctness_verified, "Sequential and concurrent scans match exactly");
    TEST_ASSERT(r_scan.seq_files_found == 50, "Generated 50 files found");

    // Watcher vs Polling experiment
    auto r_watch = bench.run_watcher_vs_polling_bench(10, 50.0);
    TEST_ASSERT(r_watch.test_file_count == 10, "10 items tested");
    TEST_ASSERT(r_watch.polling_scans > 0, "Polling scans counted");

    // Journal overhead experiment
    auto r_jrn = bench.run_journal_overhead_bench(20);
    TEST_ASSERT(r_jrn.ops_count == 20, "20 ops measured");
    TEST_ASSERT(r_jrn.recovered_ops > 0, "Simulated crash recovered items");

    // Verify benchmarks saved to SQLite
    auto hist = bench.get_history();
    TEST_ASSERT(hist.size() >= 3, "All benchmark records saved in SQLite");
    return true;
}

// ── Test 9: File Identity & Independent Histories ────────────────────
bool test_file_identity_and_histories() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    Journal j(db);

    std::string test_p = (test_sandbox / "independent_doc.txt").string();
    auto fid = j.get_or_create_file_identity(test_p);
    TEST_ASSERT(!fid.id.empty(), "FID generated");

    // Re-retrieving returns same FID
    auto fid2 = j.get_or_create_file_identity(test_p);
    TEST_ASSERT(fid.id == fid2.id, "Stable FID on path query");

    // Record Name/Location change
    NameLocationRecord nl;
    nl.id = j.generate_sequential_id("OP");
    nl.file_identity_id = fid.id;
    nl.seq = 1;
    nl.op_type = "RENAME";
    nl.source_path = test_p;
    std::string moved_p = (test_sandbox / "renamed_doc.txt").string();
    nl.dest_path = moved_p;
    j.record_name_location(nl);

    // Update identity path
    j.update_file_identity_path(fid.id, moved_p);
    auto fid_updated = j.get_file_identity(fid.id);
    TEST_ASSERT(fid_updated.has_value(), "FID found after rename");
    TEST_ASSERT(fid_updated->current_path == moved_p, "Current path updated");

    // Content revisions remain independent (0 revisions so far)
    auto revs_before = j.get_content_revisions(fid.id);
    TEST_ASSERT(revs_before.empty(), "No content revisions created by pure rename");

    // Record content revision
    ContentRevisionRecord rev1;
    rev1.id = j.generate_sequential_id("REV");
    rev1.file_identity_id = fid.id;
    rev1.revision_number = j.get_next_revision_number(fid.id);
    rev1.path = moved_p;
    rev1.size_bytes = 128;
    rev1.sha256_hash = "abc123hash";
    rev1.source_type = "USER_EDIT";
    rev1.change_summary = "Initial draft";
    j.record_content_revision(rev1);

    auto revs_after = j.get_content_revisions(fid.id);
    TEST_ASSERT(revs_after.size() == 1, "1 content revision recorded");
    TEST_ASSERT(revs_after.front().revision_number == 1, "Revision 1");

    return true;
}

// ── Test 10: External Candidates Workflow ────────────────────────────
bool test_external_candidates() {
    auto db = test_sandbox / "storage" / "journal.sqlite3";
    Journal j(db);

    std::string cand_id = j.generate_sequential_id("X");
    TEST_ASSERT(cand_id.rfind("X-", 0) == 0, "External ID format X-XXXX");

    ExternalCandidateRecord cand;
    cand.id = cand_id;
    cand.detection_seq = j.get_next_external_seq();
    cand.path = (test_sandbox / "external_file.txt").string();
    cand.change_type = "EXTERNAL_MODIFY";
    cand.detected_at = "2026-10-28T12:00:00Z";
    cand.status = "PENDING";
    cand.candidate_hash = "cand_hash_999";
    cand.candidate_size = 500;
    cand.prior_accepted_hash = "prior_hash_111";
    cand.resolution_notes = "External edit";
    j.record_external_candidate(cand);

    auto list_pending = j.list_external_candidates("PENDING");
    TEST_ASSERT(!list_pending.empty(), "Candidate listed in PENDING");

    // Reject candidate
    j.update_external_candidate_status(cand_id, "REJECTED", "Reverted external changes");
    auto cand_rej = j.get_external_candidate(cand_id);
    TEST_ASSERT(cand_rej.has_value(), "Candidate found");
    TEST_ASSERT(cand_rej->status == "REJECTED", "Status updated to REJECTED");

    // Recover rejected candidate
    j.update_external_candidate_status(cand_id, "RECOVERED", "Recovered as separate copy");
    auto cand_rec = j.get_external_candidate(cand_id);
    TEST_ASSERT(cand_rec->status == "RECOVERED", "Status updated to RECOVERED");

    return true;
}

// ── Test 11: Folder Insights & Duplicate Detection ───────────────────
bool test_folder_insights_and_duplicates() {
    auto insights_dir = test_sandbox / "insights_test";
    std::error_code ec;
    std::filesystem::create_directories(insights_dir / "sub" / "empty_dir", ec);

    FilesystemService fs({ test_sandbox });

    // Create unique file and 2 duplicate files
    std::string unique_data = "This is a unique file with specific bytes";
    std::string dup_data = "Exact duplicate payload shared across two files";

    fs.create_file(insights_dir / "unique.txt", unique_data);
    fs.create_file(insights_dir / "dup1.txt", dup_data);
    fs.create_file(insights_dir / "sub" / "dup2.txt", dup_data);

    auto insights = fs.get_folder_insights(insights_dir);
    TEST_ASSERT(insights.total_files == 3, "3 total files scanned");
    TEST_ASSERT(insights.total_directories >= 2, "Directories counted");
    TEST_ASSERT(!insights.duplicates.empty(), "Duplicate group detected");
    TEST_ASSERT(insights.duplicates.front().paths.size() == 2, "2 duplicate file paths in group");
    TEST_ASSERT(insights.reclaimable_bytes == dup_data.size(), "Reclaimable bytes computed accurately");
    TEST_ASSERT(!insights.empty_directories.empty(), "Empty directory detected");

    return true;
}

// ── Test 12: Multi-Step Undo/Redo Chain (Create -> Rename -> Overwrite -> Move) ──
bool test_multistep_undo_redo_chain() {
    auto db_path = test_sandbox / "multistep_journal.sqlite3";
    auto storage = test_sandbox / "storage";
    Journal j(db_path);
    FilesystemService fs({ test_sandbox });
    RecoveryService rec(j, fs, storage);
    CommandManager cm(j, fs, rec);

    auto sub = test_sandbox / "multistep_sub";
    std::error_code ec;
    std::filesystem::create_directories(sub, ec);

    auto f1 = test_sandbox / "chain_1.txt";
    auto f2 = test_sandbox / "chain_2.txt";
    auto f3 = sub / "chain_2.txt";

    // 1. Create file with initial content
    std::string initial_content = "Chain Initial Content";
    auto r1 = cm.execute_create_file(f1, initial_content);
    TEST_ASSERT(r1.success, "Step 1: Create file");
    TEST_ASSERT(std::filesystem::exists(f1), "f1 exists");

    // 2. Rename file1 -> file2
    auto r2 = cm.execute_rename(f1, f2);
    TEST_ASSERT(r2.success, "Step 2: Rename f1 -> f2");
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 no longer exists");
    TEST_ASSERT(std::filesystem::exists(f2), "f2 exists");

    // 3. Overwrite file2 with updated content
    std::string edited_content = "Chain Overwritten Content with extra bytes";
    std::vector<uint8_t> edit_bytes(edited_content.begin(), edited_content.end());
    auto r3 = cm.execute_overwrite(f2, edit_bytes);
    TEST_ASSERT(r3.success, "Step 3: Overwrite f2");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f2, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == edited_content, "f2 has edited content");
    }

    // 4. Move file2 -> subfolder/file2
    auto r4 = cm.execute_move(f2, f3);
    TEST_ASSERT(r4.success, "Step 4: Move f2 -> f3");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 no longer exists");
    TEST_ASSERT(std::filesystem::exists(f3), "f3 exists in subfolder");

    // Now UNDO in reverse order: 4 steps
    // Undo Step 4: Move -> should bring f3 back to f2
    auto u1 = cm.undo_last();
    TEST_ASSERT(u1.success, "Undo Move");
    TEST_ASSERT(!std::filesystem::exists(f3), "f3 absent after undo move");
    TEST_ASSERT(std::filesystem::exists(f2), "f2 present after undo move");

    // Undo Step 3: Overwrite -> should restore initial content on f2
    auto u2 = cm.undo_last();
    TEST_ASSERT(u2.success, "Undo Overwrite");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f2, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == initial_content, "f2 content restored to initial after undo overwrite");
    }

    // Undo Step 2: Rename -> should rename f2 back to f1
    auto u3 = cm.undo_last();
    TEST_ASSERT(u3.success, "Undo Rename");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 absent after undo rename");
    TEST_ASSERT(std::filesystem::exists(f1), "f1 present after undo rename");

    // Undo Step 1: Create -> should remove f1
    auto u4 = cm.undo_last();
    TEST_ASSERT(u4.success, "Undo Create");
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 removed after undo create");

    // Now REDO in forward order: 4 steps
    // Redo Step 1: Create
    auto rd1 = cm.redo_last();
    TEST_ASSERT(rd1.success, "Redo Create");
    TEST_ASSERT(std::filesystem::exists(f1), "f1 recreated");

    // Redo Step 2: Rename
    auto rd2 = cm.redo_last();
    TEST_ASSERT(rd2.success, "Redo Rename");
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 renamed away");
    TEST_ASSERT(std::filesystem::exists(f2), "f2 recreated");

    // Redo Step 3: Overwrite
    auto rd3 = cm.redo_last();
    TEST_ASSERT(rd3.success, "Redo Overwrite");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f2, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == edited_content, "f2 has edited content after redo overwrite");
    }

    // Redo Step 4: Move
    auto rd4 = cm.redo_last();
    TEST_ASSERT(rd4.success, "Redo Move");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 moved away");
    TEST_ASSERT(std::filesystem::exists(f3), "f3 present in subfolder after redo move");

    return true;
}

// ── Test 13: External Rename Handling & Physical Disk Rejection ─────
bool test_external_rename_rejection() {
    auto db_path = test_sandbox / "ext_rename_journal.sqlite3";
    Journal j(db_path);
    FilesystemService fs({ test_sandbox });

    auto orig_file = test_sandbox / "original_doc.txt";
    auto renamed_file = test_sandbox / "renamed_doc.txt";

    // Simulate file created then externally renamed
    fs.create_file(renamed_file, "External rename test content");
    TEST_ASSERT(std::filesystem::exists(renamed_file), "Renamed file on disk");
    TEST_ASSERT(!std::filesystem::exists(orig_file), "Original file absent");

    ExternalCandidateRecord cand;
    cand.id = "X-TEST-RENAME";
    cand.detection_seq = 1;
    cand.path = renamed_file.string();
    cand.old_path = orig_file.string();
    cand.change_type = "EXTERNAL_RENAME";
    cand.detected_at = "2026-03-30T12:00:00Z";
    cand.status = "PENDING";
    cand.candidate_hash = sha256_file(renamed_file);
    cand.candidate_size = std::filesystem::file_size(renamed_file);
    j.record_external_candidate(cand);

    auto cand_opt = j.get_external_candidate(cand.id);
    TEST_ASSERT(cand_opt.has_value(), "Candidate found in SQLite");
    TEST_ASSERT(cand_opt->old_path == orig_file.string(), "old_path preserved in SQLite");

    // Rejection execution: physically rename back to old_path
    fs.rename_item(cand_opt->path, cand_opt->old_path);
    j.update_external_candidate_status(cand.id, "REJECTED", "Reverted external rename back to original name");

    TEST_ASSERT(!std::filesystem::exists(renamed_file), "Renamed file absent after rejection");
    TEST_ASSERT(std::filesystem::exists(orig_file), "Original filename physically restored on disk");

    auto updated = j.get_external_candidate(cand.id);
    TEST_ASSERT(updated->status == "REJECTED", "Candidate status marked REJECTED");

    return true;
}

// ── Test 14: Rename -> Edit -> Undo -> Redo Sequence with Persistent Identity ──
bool test_rename_edit_undo_redo_sequence() {
    auto db_path = test_sandbox / "rename_edit_journal.sqlite3";
    auto storage = test_sandbox / "storage_rename_edit";
    Journal j(db_path);
    FilesystemService fs({ test_sandbox });
    RecoveryService rec(j, fs, storage);
    CommandManager cm(j, fs, rec);

    auto f_alpha = test_sandbox / "alpha.txt";
    auto f_beta  = test_sandbox / "beta.txt";

    // Step 1: Create file "alpha.txt" with content "Version 1"
    std::string v1 = "Version 1";
    auto r1 = cm.execute_create_file(f_alpha, v1);
    TEST_ASSERT(r1.success, "Step 1: Create alpha.txt");
    TEST_ASSERT(std::filesystem::exists(f_alpha), "alpha.txt exists");

    // Step 2: Rename "alpha.txt" to "beta.txt"
    auto r2 = cm.execute_rename(f_alpha, f_beta);
    TEST_ASSERT(r2.success, "Step 2: Rename alpha.txt -> beta.txt");
    TEST_ASSERT(!std::filesystem::exists(f_alpha), "alpha.txt gone");
    TEST_ASSERT(std::filesystem::exists(f_beta), "beta.txt exists");

    // Step 3: Overwrite "beta.txt" with content "Version 2"
    std::string v2 = "Version 2";
    std::vector<uint8_t> v2_bytes(v2.begin(), v2.end());
    auto r3 = cm.execute_overwrite(f_beta, v2_bytes);
    TEST_ASSERT(r3.success, "Step 3: Overwrite beta.txt with Version 2");

    // Step 4: Verify file on disk is beta.txt with content "Version 2"
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f_beta, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == v2, "Step 4: beta.txt content is Version 2");
    }

    // Step 5: Undo (should undo Step 3: Overwrite)
    auto u1 = cm.undo_last();
    TEST_ASSERT(u1.success, "Step 5: Undo overwrite");

    // Step 6: Verify beta.txt exists with "Version 1"
    TEST_ASSERT(std::filesystem::exists(f_beta), "Step 6: beta.txt still exists");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f_beta, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == v1, "Step 6: beta.txt content restored to Version 1");
    }

    // Step 7: Undo again (should undo Step 2: Rename)
    auto u2 = cm.undo_last();
    TEST_ASSERT(u2.success, "Step 7: Undo rename");

    // Step 8: Verify alpha.txt exists with "Version 1", beta.txt does not exist
    TEST_ASSERT(std::filesystem::exists(f_alpha), "Step 8: alpha.txt restored");
    TEST_ASSERT(!std::filesystem::exists(f_beta), "Step 8: beta.txt gone");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f_alpha, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == v1, "Step 8: alpha.txt content is Version 1");
    }

    // Step 9: Redo (should redo Step 2: Rename)
    auto rd1 = cm.redo_last();
    TEST_ASSERT(rd1.success, "Step 9: Redo rename");

    // Step 10: Verify beta.txt exists with "Version 1"
    TEST_ASSERT(!std::filesystem::exists(f_alpha), "Step 10: alpha.txt gone after redo rename");
    TEST_ASSERT(std::filesystem::exists(f_beta), "Step 10: beta.txt exists after redo rename");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f_beta, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == v1, "Step 10: beta.txt content is Version 1");
    }

    // Step 11: Redo again (should redo Step 3: Overwrite)
    auto rd2 = cm.redo_last();
    TEST_ASSERT(rd2.success, "Step 11: Redo overwrite");

    // Step 12: Verify beta.txt exists with content "Version 2"
    TEST_ASSERT(std::filesystem::exists(f_beta), "Step 12: beta.txt exists after redo overwrite");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(f_beta, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == v2, "Step 12: beta.txt content is Version 2");
    }

    return true;
}

// ── Test 15: Quarantine Advanced Operations, Collisions & Permanent Delete ──
bool test_quarantine_advanced_operations() {
    auto db_path = test_sandbox / "quarantine_adv_journal.sqlite3";
    auto storage = test_sandbox / "storage_quarantine_adv";
    Journal j(db_path);
    FilesystemService fs({ test_sandbox });
    RecoveryService rec(j, fs, storage);
    CommandManager cm(j, fs, rec);

    auto target_file = test_sandbox / "suspicious_payload.bin";
    std::string payload = "MALICIOUS_SIMULATION_ENTROPY_DATA_12345";
    fs.create_file(target_file, payload);
    TEST_ASSERT(std::filesystem::exists(target_file), "Payload file created");

    // Execute quarantine via CommandManager
    auto q_res = cm.execute_quarantine(target_file, "High entropy payload detected");
    TEST_ASSERT(q_res.success, "Quarantine command succeeded");
    TEST_ASSERT(!std::filesystem::exists(target_file), "Target removed from original path");

    // List quarantine entries
    auto q_list = rec.list_quarantine();
    TEST_ASSERT(!q_list.empty(), "Quarantine vault has entries");
    std::string qid = q_list.front().id;
    TEST_ASSERT(q_list.front().original_path == target_file.string(), "Original path recorded");
    auto op = j.get_operation(q_res.operation_id);
    TEST_ASSERT(op.has_value(), "Operation recorded in journal");
    auto det = json::parse(op->details_json);
    TEST_ASSERT(det.value("reason", "") == "High entropy payload detected", "Reason recorded");

    // Collision detection test: create a new file at original location
    std::string decoy = "Decoy file occupying original spot";
    fs.create_file(target_file, decoy);
    TEST_ASSERT(std::filesystem::exists(target_file), "Decoy file created at original location");

    // Attempt restore without overwrite -> must fail with collision
    bool collision_caught = false;
    try {
        rec.restore_quarantined_item(qid, false, "");
    } catch (const std::exception& e) {
        collision_caught = true;
    }
    TEST_ASSERT(collision_caught, "Restore conflict caught on occupied destination");

    // Restore to custom destination
    auto custom_dest = test_sandbox / "restored_safe_payload.bin";
    bool custom_restored = rec.restore_quarantined_item(qid, false, custom_dest.string());
    TEST_ASSERT(custom_restored, "Restored to custom destination");
    TEST_ASSERT(std::filesystem::exists(custom_dest), "File restored at custom location");
    {
        std::vector<uint8_t> read_bytes;
        fs.read_file(custom_dest, read_bytes);
        std::string s(read_bytes.begin(), read_bytes.end());
        TEST_ASSERT(s == payload, "Custom destination content matches quarantined payload");
    }

    // Now test permanent deletion on a quarantined item
    auto q_res2 = cm.execute_quarantine(custom_dest, "Permanent delete test");
    TEST_ASSERT(q_res2.success, "Second quarantine succeeded");
    auto q_list2 = rec.list_quarantine();
    std::string qid2 = q_list2.front().id;

    bool perm_del = rec.permanently_delete_quarantined_item(qid2);
    TEST_ASSERT(perm_del, "Permanent delete returned true");
    auto q_entry_check = rec.get_quarantine_item(qid2);
    TEST_ASSERT(!q_entry_check.has_value(), "Entry completely deleted from database");

    return true;
}

// ── Test 16: Batch Transactions, Child Operation Linkage & Full Rollback ──
bool test_batch_transaction_details_and_undo() {
    auto db_path = test_sandbox / "batch_tx_journal.sqlite3";
    auto storage = test_sandbox / "storage_batch_tx";
    Journal j(db_path);
    FilesystemService fs({ test_sandbox });
    RecoveryService rec(j, fs, storage);
    CommandManager cm(j, fs, rec);

    auto src_dir = test_sandbox / "batch_src";
    auto dst_dir = test_sandbox / "batch_dst";
    std::error_code ec;
    std::filesystem::create_directories(src_dir, ec);
    std::filesystem::create_directories(dst_dir, ec);

    auto f1 = src_dir / "item1.txt";
    auto f2 = src_dir / "item2.txt";
    auto f3 = src_dir / "item3.txt";

    fs.create_file(f1, "Item 1 content");
    fs.create_file(f2, "Item 2 content");
    fs.create_file(f3, "Item 3 content");

    std::vector<BatchItem> move_items = {
        { f1.string(), (dst_dir / "item1.txt").string() },
        { f2.string(), (dst_dir / "item2.txt").string() },
        { f3.string(), (dst_dir / "item3.txt").string() }
    };

    auto batch_res = cm.execute_batch_move(move_items);
    TEST_ASSERT(batch_res.success, "Batch move executed successfully");
    std::string tx_id = batch_res.operation_id;
    TEST_ASSERT(!tx_id.empty(), "Batch move returned transaction ID");

    // Verify child operations in SQLite
    auto child_ops = j.get_operations_for_transaction(tx_id);
    TEST_ASSERT(child_ops.size() == 3, "Exactly 3 child operations linked to transaction ID");
    for (const auto& cop : child_ops) {
        TEST_ASSERT(cop.transaction_id == tx_id, "Child operation has matching transaction_id");
        TEST_ASSERT(cop.type == OpType::OpMove, "Child operation type is MOVE");
    }

    // Verify files on disk in destination
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 moved away from src");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 moved away from src");
    TEST_ASSERT(!std::filesystem::exists(f3), "f3 moved away from src");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "item1.txt"), "item1 in dst");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "item2.txt"), "item2 in dst");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "item3.txt"), "item3 in dst");

    // Perform transaction undo / rollback
    auto undo_tx_res = cm.undo_transaction(tx_id);
    TEST_ASSERT(undo_tx_res.success, "Batch transaction undo succeeded");

    // Verify all 3 files are restored in source directory
    TEST_ASSERT(std::filesystem::exists(f1), "f1 restored in src");
    TEST_ASSERT(std::filesystem::exists(f2), "f2 restored in src");
    TEST_ASSERT(std::filesystem::exists(f3), "f3 restored in src");
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "item1.txt"), "item1 removed from dst");
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "item2.txt"), "item2 removed from dst");
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "item3.txt"), "item3 removed from dst");

    // Verify transaction status is ROLLED_BACK
    auto tx_opt = j.get_transaction(tx_id);
    TEST_ASSERT(tx_opt.has_value(), "Transaction found in SQLite");
    TEST_ASSERT(tx_opt->status == "ROLLED_BACK", "Transaction status is ROLLED_BACK");

    return true;
}

// ── Test 17: Rename Preserves Complete File History ──────────────────
bool test_rename_preserves_complete_file_history() {
    auto db_p = test_sandbox / "storage" / "safevault.sqlite3";
    Journal j(db_p);
    j.init_schema();
    std::vector<std::filesystem::path> roots = { test_sandbox };
    FilesystemService fs(roots);
    RecoveryService rec(j, fs, test_sandbox / "storage");
    CommandManager cm(j, fs, rec);

    auto f1 = test_sandbox / "rp1.txt";
    auto f2 = test_sandbox / "rp2.txt";

    // 1. Create rp1.txt
    auto res_create = cm.execute_create_file(f1, "Initial Content v1");
    TEST_ASSERT(res_create.success, "Create rp1.txt succeeded");

    // 2. Edit rp1.txt twice
    std::vector<uint8_t> v2 = {'I', 'n', 'i', 't', 'i', 'a', 'l', ' ', 'v', '2'};
    auto res_edit1 = cm.execute_overwrite(f1, v2);
    TEST_ASSERT(res_edit1.success, "Edit rp1.txt to v2");

    std::vector<uint8_t> v3 = {'I', 'n', 'i', 't', 'i', 'a', 'l', ' ', 'v', '3'};
    auto res_edit2 = cm.execute_overwrite(f1, v3);
    TEST_ASSERT(res_edit2.success, "Edit rp1.txt to v3");

    // 3. Rename rp1.txt -> rp2.txt
    auto res_rename = cm.execute_rename(f1, f2);
    TEST_ASSERT(res_rename.success, "Rename rp1.txt -> rp2.txt");

    // 4. Edit rp2.txt
    std::vector<uint8_t> v4 = {'E', 'd', 'i', 't', 'e', 'd', ' ', 'v', '4'};
    auto res_edit3 = cm.execute_overwrite(f2, v4);
    TEST_ASSERT(res_edit3.success, "Edit rp2.txt to v4");

    // 5. Query history for rp2.txt
    auto fid_opt = j.find_file_identity_by_path(f2.string());
    TEST_ASSERT(fid_opt.has_value(), "File identity found for rp2.txt");

    // Stable file identity: initial_path was rp1.txt, current_path is rp2.txt
    TEST_ASSERT(fid_opt->current_path == f2.string(), "Current path is rp2.txt");
    TEST_ASSERT(fid_opt->initial_path == f1.string(), "Initial path was rp1.txt");

    // Content revisions: all 4 revisions must exist
    auto revs = j.get_content_revisions_for_path(f2.string());
    TEST_ASSERT(revs.size() == 4, "All 4 revisions preserved across rename");
    TEST_ASSERT(revs[0].revision_number == 4, "Latest rev is 4");
    TEST_ASSERT(revs[3].revision_number == 1, "Earliest rev is 1");

    // Name location history: must show CREATE and RENAME
    auto nls = j.get_name_location_history_for_path(f2.string());
    TEST_ASSERT(nls.size() >= 2, "Name location history has entries");
    bool has_rename = false;
    for (const auto& nl : nls) {
        if (nl.op_type == "RENAME") has_rename = true;
    }
    TEST_ASSERT(has_rename, "Rename record found in name location history");

    // Operations journal for rp2.txt must include create, edits, and rename
    auto ops = j.get_operations_for_file(f2.string(), 100);
    TEST_ASSERT(ops.size() >= 5, "Operations list contains full history of operations across rename");

    return true;
}

// ── Test 18: Independent Content and Name/Location Undo/Redo ────────
bool test_independent_content_and_name_undo_redo() {
    auto db_p = test_sandbox / "storage" / "safevault.sqlite3";
    Journal j(db_p);
    j.init_schema();
    std::vector<std::filesystem::path> roots = { test_sandbox };
    FilesystemService fs(roots);
    RecoveryService rec(j, fs, test_sandbox / "storage");
    CommandManager cm(j, fs, rec);

    auto f1 = test_sandbox / "rp1_ind.txt";
    auto f2 = test_sandbox / "rp2_ind.txt";

    // 1. rp1_ind.txt with "Version A"
    auto r1 = cm.execute_create_file(f1, "Version A");
    TEST_ASSERT(r1.success, "Create rp1_ind.txt");

    // 2. Rename to rp2_ind.txt
    auto r2 = cm.execute_rename(f1, f2);
    TEST_ASSERT(r2.success, "Rename rp1_ind.txt -> rp2_ind.txt");

    // 3. Edit to "Version B"
    std::vector<uint8_t> vb = {'V', 'e', 'r', 's', 'i', 'o', 'n', ' ', 'B'};
    auto r3 = cm.execute_overwrite(f2, vb);
    TEST_ASSERT(r3.success, "Edit rp2_ind.txt to Version B");

    TEST_ASSERT(cm.can_undo_content(), "can_undo_content is true");
    TEST_ASSERT(cm.can_undo_name(), "can_undo_name is true");

    // 4. Execute Content Undo: verify rp2_ind.txt exists with Version A
    auto undo_c = cm.undo_content();
    TEST_ASSERT(undo_c.success, "Content undo succeeded");
    TEST_ASSERT(std::filesystem::exists(f2), "rp2_ind.txt still exists at current path");
    std::string text_after_c_undo;
    {
        std::ifstream in(f2, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        text_after_c_undo = ss.str();
    }
    TEST_ASSERT(text_after_c_undo == "Version A", "rp2_ind.txt content reverted to Version A");

    // 5. Execute Name Undo: verify rp1_ind.txt exists with Version A
    auto undo_n = cm.undo_name();
    TEST_ASSERT(undo_n.success, "Name undo succeeded");
    TEST_ASSERT(std::filesystem::exists(f1), "rp1_ind.txt exists at restored original path");
    TEST_ASSERT(!std::filesystem::exists(f2), "rp2_ind.txt no longer exists");
    std::string text_after_n_undo;
    {
        std::ifstream in(f1, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        text_after_n_undo = ss.str();
    }
    TEST_ASSERT(text_after_n_undo == "Version A", "rp1_ind.txt content is Version A");

    // 6. Execute Name Redo: verify rp2_ind.txt exists with Version A
    TEST_ASSERT(cm.can_redo_name(), "can_redo_name is true");
    auto redo_n = cm.redo_name();
    TEST_ASSERT(redo_n.success, "Name redo succeeded");
    TEST_ASSERT(std::filesystem::exists(f2), "rp2_ind.txt exists after name redo");
    TEST_ASSERT(!std::filesystem::exists(f1), "rp1_ind.txt no longer exists");
    std::string text_after_n_redo;
    {
        std::ifstream in(f2, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        text_after_n_redo = ss.str();
    }
    TEST_ASSERT(text_after_n_redo == "Version A", "rp2_ind.txt content is Version A");

    // 7. Execute Content Redo: verify rp2_ind.txt exists with Version B
    TEST_ASSERT(cm.can_redo_content(), "can_redo_content is true");
    auto redo_c = cm.redo_content();
    TEST_ASSERT(redo_c.success, "Content redo succeeded");
    TEST_ASSERT(std::filesystem::exists(f2), "rp2_ind.txt exists");
    std::string text_after_c_redo;
    {
        std::ifstream in(f2, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        text_after_c_redo = ss.str();
    }
    TEST_ASSERT(text_after_c_redo == "Version B", "rp2_ind.txt content restored to Version B");

    return true;
}

// ── Test 19: Batch Move Multi-Folder Restoration ─────────────────────
bool test_batch_move_multi_folder_restoration() {
    auto db_p = test_sandbox / "storage" / "safevault.sqlite3";
    Journal j(db_p);
    j.init_schema();
    std::vector<std::filesystem::path> roots = { test_sandbox };
    FilesystemService fs(roots);
    RecoveryService rec(j, fs, test_sandbox / "storage");
    CommandManager cm(j, fs, rec);

    auto dir_a = test_sandbox / "FolderA";
    auto dir_b = test_sandbox / "FolderB";
    auto dir_c = test_sandbox / "FolderC";
    auto dir_d = test_sandbox / "FolderD";

    fs.create_directory(dir_a);
    fs.create_directory(dir_b);
    fs.create_directory(dir_c);
    fs.create_directory(dir_d);

    auto f1 = dir_a / "f1.txt";
    auto f2 = dir_b / "f2.txt";
    auto f3 = dir_c / "f3.txt";

    fs.create_file(f1, "File 1 from FolderA");
    fs.create_file(f2, "File 2 from FolderB");
    fs.create_file(f3, "File 3 from FolderC");

    std::vector<BatchItem> items = {
        {f1.string(), (dir_d / "f1.txt").string()},
        {f2.string(), (dir_d / "f2.txt").string()},
        {f3.string(), (dir_d / "f3.txt").string()}
    };

    auto res = cm.execute_batch_move(items);
    TEST_ASSERT(res.success, "Batch move of multiple folders into FolderD succeeded");
    std::string tx_id = res.operation_id;

    TEST_ASSERT(std::filesystem::exists(dir_d / "f1.txt"), "f1 in FolderD");
    TEST_ASSERT(std::filesystem::exists(dir_d / "f2.txt"), "f2 in FolderD");
    TEST_ASSERT(std::filesystem::exists(dir_d / "f3.txt"), "f3 in FolderD");
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 removed from FolderA");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 removed from FolderB");
    TEST_ASSERT(!std::filesystem::exists(f3), "f3 removed from FolderC");

    // Undo batch move: all files must return to their exact individual original folders
    auto undo_res = cm.undo_transaction(tx_id);
    TEST_ASSERT(undo_res.success, "Undo batch move transaction succeeded");

    TEST_ASSERT(std::filesystem::exists(f1), "f1 restored to FolderA");
    TEST_ASSERT(std::filesystem::exists(f2), "f2 restored to FolderB");
    TEST_ASSERT(std::filesystem::exists(f3), "f3 restored to FolderC");
    TEST_ASSERT(!std::filesystem::exists(dir_d / "f1.txt"), "f1 removed from FolderD");
    TEST_ASSERT(!std::filesystem::exists(dir_d / "f2.txt"), "f2 removed from FolderD");
    TEST_ASSERT(!std::filesystem::exists(dir_d / "f3.txt"), "f3 removed from FolderD");

    return true;
}

// ── Test 20: Per-File External Candidate Archive Across Renames ──────
bool test_per_file_external_candidate_archive_across_renames() {
    auto db_p = test_sandbox / "storage" / "safevault.sqlite3";
    Journal j(db_p);
    j.init_schema();
    std::vector<std::filesystem::path> roots = { test_sandbox };
    FilesystemService fs(roots);
    RecoveryService rec(j, fs, test_sandbox / "storage");
    CommandManager cm(j, fs, rec);

    auto f1 = test_sandbox / "ext_orig.txt";
    auto f2 = test_sandbox / "ext_renamed.txt";

    // 1. Create file ext_orig.txt
    cm.execute_create_file(f1, "Original SafeVault content");
    auto fid = j.get_or_create_file_identity(f1.string());

    // 2. Record external candidate C1 under ext_orig.txt and reject it
    ExternalCandidateRecord c1;
    c1.id = j.generate_sequential_id("X");
    c1.detection_seq = j.get_next_external_seq();
    c1.path = f1.string();
    c1.old_path = "";
    c1.file_identity_id = fid.id;
    c1.change_type = "EXTERNAL_MODIFY";
    c1.detected_at = "2026-10-29T10:00:00Z";
    c1.status = "REJECTED";
    c1.candidate_hash = "hash_c1";
    c1.resolution_notes = "Rejected external edit 1";
    j.record_external_candidate(c1);

    // 3. Rename file to ext_renamed.txt
    cm.execute_rename(f1, f2);

    // 4. Record external candidate C2 under ext_renamed.txt and reject it
    ExternalCandidateRecord c2;
    c2.id = j.generate_sequential_id("X");
    c2.detection_seq = j.get_next_external_seq();
    c2.path = f2.string();
    c2.old_path = "";
    c2.file_identity_id = fid.id;
    c2.change_type = "EXTERNAL_MODIFY";
    c2.detected_at = "2026-10-29T11:00:00Z";
    c2.status = "REJECTED";
    c2.candidate_hash = "hash_c2";
    c2.resolution_notes = "Rejected external edit 2";
    j.record_external_candidate(c2);

    // 5. Query per-file external candidates for ext_renamed.txt
    auto cands = j.get_external_candidates_for_path(f2.string());
    TEST_ASSERT(cands.size() >= 2, "Both external candidates found for renamed file");

    bool has_c1 = false;
    bool has_c2 = false;
    for (const auto& c : cands) {
        if (c.id == c1.id) has_c1 = true;
        if (c.id == c2.id) has_c2 = true;
    }
    TEST_ASSERT(has_c1, "Candidate C1 from original name is preserved in archive");
    TEST_ASSERT(has_c2, "Candidate C2 from renamed file is present in archive");

    return true;
}

// ── Test 21: Batch Transaction Atomic Undo & Redo ─────────────────────
bool test_batch_transaction_atomic_undo_and_redo() {
    auto db_p = test_sandbox / "storage" / "safevault.sqlite3";
    Journal j(db_p);
    j.init_schema();
    std::vector<std::filesystem::path> roots = { test_sandbox };
    FilesystemService fs(roots);
    RecoveryService rec(j, fs, test_sandbox / "storage");
    CommandManager cm(j, fs, rec);

    auto src_dir = test_sandbox / "batch_atomic_src";
    auto dst_dir = test_sandbox / "batch_atomic_dst";
    std::error_code ec;
    std::filesystem::create_directories(src_dir, ec);
    std::filesystem::create_directories(dst_dir, ec);

    auto f1 = src_dir / "doc1.txt";
    auto f2 = src_dir / "doc2.txt";
    fs.create_file(f1, "Doc 1 data");
    fs.create_file(f2, "Doc 2 data");

    // 1. Batch move doc1 and doc2
    std::vector<BatchItem> move_items = {
        { f1.string(), (dst_dir / "doc1.txt").string() },
        { f2.string(), (dst_dir / "doc2.txt").string() }
    };
    auto b_res = cm.execute_batch_move(move_items);
    TEST_ASSERT(b_res.success, "Batch move succeeded");
    std::string tx_id = b_res.operation_id;
    TEST_ASSERT(!tx_id.empty(), "Batch move returned tx_id");
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 moved");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 moved");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "doc1.txt"), "doc1 in dst");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "doc2.txt"), "doc2 in dst");

    // 2. Calling undo_name() must recognize transaction_id and undo the entire batch atomically
    auto undo_res = cm.undo_name();
    TEST_ASSERT(undo_res.success, "undo_name succeeded for batch transaction");
    TEST_ASSERT(std::filesystem::exists(f1), "f1 atomically restored to src");
    TEST_ASSERT(std::filesystem::exists(f2), "f2 atomically restored to src");
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "doc1.txt"), "doc1 removed from dst");
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "doc2.txt"), "doc2 removed from dst");

    auto tx_rec = j.get_transaction(tx_id);
    TEST_ASSERT(tx_rec.has_value(), "Transaction record found");
    TEST_ASSERT(tx_rec->status == "ROLLED_BACK", "Transaction marked ROLLED_BACK");

    // 3. Calling redo_name() must reapply the entire batch atomically
    auto redo_res = cm.redo_name();
    TEST_ASSERT(redo_res.success, "redo_name succeeded for batch transaction");
    TEST_ASSERT(!std::filesystem::exists(f1), "f1 moved again by redo");
    TEST_ASSERT(!std::filesystem::exists(f2), "f2 moved again by redo");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "doc1.txt"), "doc1 reapplied to dst");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "doc2.txt"), "doc2 reapplied to dst");

    tx_rec = j.get_transaction(tx_id);
    TEST_ASSERT(tx_rec.has_value(), "Transaction record found");
    TEST_ASSERT(tx_rec->status == "COMPLETED", "Transaction marked COMPLETED after redo");

    // 4. Batch delete and direct undo_transaction
    std::vector<std::string> del_paths = { (dst_dir / "doc1.txt").string(), (dst_dir / "doc2.txt").string() };
    auto del_batch_res = cm.execute_batch_delete(del_paths);
    TEST_ASSERT(del_batch_res.success, "Batch delete succeeded");
    std::string del_tx_id = del_batch_res.operation_id;
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "doc1.txt"), "doc1 deleted");
    TEST_ASSERT(!std::filesystem::exists(dst_dir / "doc2.txt"), "doc2 deleted");

    auto undo_del_res = cm.undo_transaction(del_tx_id);
    TEST_ASSERT(undo_del_res.success, "undo_transaction on batch delete succeeded");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "doc1.txt"), "doc1 restored from quarantine");
    TEST_ASSERT(std::filesystem::exists(dst_dir / "doc2.txt"), "doc2 restored from quarantine");

    return true;
}

int run_all_tests() {
    setup_sandbox();

    std::cout << "========================================\n";
    std::cout << "  FileGuard Automated Test Suite\n";
    std::cout << "========================================\n";

    int passed = 0;
    int failed = 0;

    RUN_TEST(test_sha256);
    RUN_TEST(test_filesystem_operations);
    RUN_TEST(test_undo_redo_primitives);
    RUN_TEST(test_persistent_restart);
    RUN_TEST(test_redo_branch_invalidation);
    RUN_TEST(test_conflict_detection);
    RUN_TEST(test_batch_transactions);
    RUN_TEST(test_benchmarks);
    RUN_TEST(test_file_identity_and_histories);
    RUN_TEST(test_external_candidates);
    RUN_TEST(test_folder_insights_and_duplicates);
    RUN_TEST(test_multistep_undo_redo_chain);
    RUN_TEST(test_external_rename_rejection);
    RUN_TEST(test_rename_edit_undo_redo_sequence);
    RUN_TEST(test_quarantine_advanced_operations);
    RUN_TEST(test_batch_transaction_details_and_undo);
    RUN_TEST(test_rename_preserves_complete_file_history);
    RUN_TEST(test_independent_content_and_name_undo_redo);
    RUN_TEST(test_batch_move_multi_folder_restoration);
    RUN_TEST(test_per_file_external_candidate_archive_across_renames);
    RUN_TEST(test_batch_transaction_atomic_undo_and_redo);

    std::cout << "========================================\n";
    std::cout << "Tests Passed: " << passed << " / " << (passed + failed) << "\n";
    std::cout << "========================================\n";

    cleanup_sandbox();
    return failed == 0 ? 0 : 1;
}
} // namespace fileguard

#ifndef FILEGUARD_NO_TEST_MAIN
int main() {
    return fileguard::run_all_tests();
}
#endif
