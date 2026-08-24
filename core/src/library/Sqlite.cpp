#include "Sqlite.hpp"

#include "xpcog/core/FilePath.hpp"

#include <cstddef>

namespace xpcog::sql {

// --- Database -----------------------------------------------------------

bool Database::open(const std::filesystem::path& path) {
    close();

    // sqlite3 takes UTF-8 filenames on every platform, including Windows where
    // path.string() would hand it the active code page instead.
    const std::string text = pathToUtf8(path);
    const int         status =
        sqlite3_open_v2(text.c_str(), &handle_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (status != SQLITE_OK) {
        // sqlite3_open_v2 hands back a handle even on failure, so the error text
        // can be read -- but it still has to be closed.
        close();
        return false;
    }

    // WAL lets the scanner write while the UI reads. NORMAL trades an fsync per
    // commit for the possibility of losing the last transaction on power loss,
    // which for a playlist is the right trade.
    return exec("PRAGMA journal_mode = WAL;"
                "PRAGMA synchronous = NORMAL;"
                "PRAGMA foreign_keys = ON;");
}

bool Database::openReadOnly(const std::filesystem::path& path) {
    close();

    const std::string text = pathToUtf8(path);
    if (sqlite3_open_v2(text.c_str(), &handle_, SQLITE_OPEN_READONLY, nullptr) !=
        SQLITE_OK) {
        close();
        return false;
    }
    return true;
}

void Database::close() {
    if (handle_ != nullptr) {
        sqlite3_close_v2(handle_);
        handle_ = nullptr;
    }
}

bool Database::exec(std::string_view sql) {
    if (handle_ == nullptr) {
        return false;
    }
    const std::string text = std::string{sql};
    return sqlite3_exec(handle_, text.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::int64_t Database::lastInsertRowId() const {
    return (handle_ != nullptr) ? sqlite3_last_insert_rowid(handle_) : 0;
}

std::string Database::lastError() const {
    return (handle_ != nullptr) ? sqlite3_errmsg(handle_) : "database is not open";
}

// --- Statement ----------------------------------------------------------

Statement::Statement(Database& database, std::string_view sql) {
    if (!database.isOpen()) {
        return;
    }
    if (sqlite3_prepare_v2(database.handle(), sql.data(),
                           static_cast<int>(sql.size()), &statement_,
                           nullptr) != SQLITE_OK) {
        statement_ = nullptr;
    }
}

Statement::~Statement() {
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

void Statement::bind(int index, std::int64_t value) {
    if (statement_ != nullptr) {
        sqlite3_bind_int64(statement_, index, value);
    }
}

void Statement::bind(int index, double value) {
    if (statement_ != nullptr) {
        sqlite3_bind_double(statement_, index, value);
    }
}

void Statement::bind(int index, std::string_view value) {
    if (statement_ != nullptr) {
        // SQLITE_TRANSIENT: sqlite copies. The alternative saves a copy and
        // requires the caller's string to outlive the step, which is exactly the
        // kind of lifetime rule that eventually gets broken.
        sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
}

void Statement::bind(int index, std::span<const std::byte> value) {
    if (statement_ != nullptr) {
        sqlite3_bind_blob(statement_, index, value.data(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT);
    }
}

void Statement::bindNull(int index) {
    if (statement_ != nullptr) {
        sqlite3_bind_null(statement_, index);
    }
}

bool Statement::step() {
    if (statement_ == nullptr) {
        return false;
    }
    return sqlite3_step(statement_) == SQLITE_ROW;
}

bool Statement::run() {
    if (statement_ == nullptr) {
        return false;
    }
    const int status = sqlite3_step(statement_);
    return status == SQLITE_DONE || status == SQLITE_ROW;
}

void Statement::reset() {
    if (statement_ != nullptr) {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }
}

bool Statement::isNull(int column) const {
    return statement_ == nullptr ||
           sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

std::int64_t Statement::columnInt(int column) const {
    return (statement_ != nullptr) ? sqlite3_column_int64(statement_, column) : 0;
}

double Statement::columnDouble(int column) const {
    return (statement_ != nullptr) ? sqlite3_column_double(statement_, column) : 0.0;
}

std::string Statement::columnText(int column) const {
    if (statement_ == nullptr) {
        return {};
    }
    const auto* text = sqlite3_column_text(statement_, column);
    if (text == nullptr) {
        return {};
    }
    const int size = sqlite3_column_bytes(statement_, column);
    return std::string{reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(size)};
}

std::vector<std::byte> Statement::columnBlob(int column) const {
    if (statement_ == nullptr) {
        return {};
    }
    const void* data = sqlite3_column_blob(statement_, column);
    const int   size = sqlite3_column_bytes(statement_, column);
    if (data == nullptr || size <= 0) {
        return {};
    }
    const auto* bytes = static_cast<const std::byte*>(data);
    return std::vector<std::byte>{bytes, bytes + size};
}

// --- Transaction --------------------------------------------------------

Transaction::Transaction(Database& database) : database_(&database) {
    active_ = database.exec("BEGIN IMMEDIATE;");
}

Transaction::~Transaction() {
    if (active_) {
        // Nobody committed, so something failed on the way out. Rolling back is
        // the only safe interpretation.
        static_cast<void>(database_->exec("ROLLBACK;"));
    }
}

bool Transaction::commit() {
    if (!active_) {
        return false;
    }
    active_ = false;
    return database_->exec("COMMIT;");
}

}  // namespace xpcog::sql
