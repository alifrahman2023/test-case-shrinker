#include "saving.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <unistd.h>

namespace {
struct SaveDirectory {
    std::filesystem::path path;
    SaveDirectory() { char pattern[] = "/tmp/shrink-save-XXXXXX"; path = mkdtemp(pattern); }
    ~SaveDirectory() { std::filesystem::remove_all(path); }
    std::string read() const {
        std::ifstream file(path / "result"); return {std::istreambuf_iterator<char>(file), {}};
    }
};
}
TEST_CASE("atomic saving replaces only after a complete temporary write", "[saving]") {
    using namespace shrink;
    SaveDirectory directory;
    { std::ofstream file(directory.path / "result"); file << "previous"; }
    cli::SaveFileOperations operations;
    operations.write = [](int, const void*, std::size_t) -> ssize_t { errno = ENOSPC; return -1; };
    auto error = cli::save_atomic(directory.path / "result", {"3\n7 -2 7\n", 1, SaveKind::initial}, operations);
    REQUIRE(error);
    REQUIRE(error->code == SaveErrorCode::write_temporary);
    REQUIRE(error->system_error.value() == ENOSPC);
    REQUIRE(error->path == directory.path / "result");
    REQUIRE(directory.read() == "previous");
    REQUIRE(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == 1);
    REQUIRE_FALSE(cli::save_atomic(directory.path / "result", {"3\n7 -2 7\n", 1, SaveKind::final}));
    REQUIRE(directory.read() == "3\n7 -2 7\n");
}
TEST_CASE("rename failure leaves the previous result and removes the temporary file", "[saving]") {
    using namespace shrink;
    SaveDirectory directory;
    { std::ofstream file(directory.path / "result"); file << "previous"; }
    cli::SaveFileOperations operations;
    operations.rename = [](const char*, const char*) { errno = EACCES; return -1; };
    auto error = cli::save_atomic(directory.path / "result", {"0\n\n", 1, SaveKind::final}, operations);
    REQUIRE(error->code == SaveErrorCode::replace);
    REQUIRE(error->system_error.value() == EACCES);
    REQUIRE(directory.read() == "previous");
    REQUIRE(std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{}) == 1);
}
TEST_CASE("atomic saving completes partial writes and interrupted transfers", "[saving]") {
    using namespace shrink;
    SaveDirectory directory;
    unsigned calls = 0;
    cli::SaveFileOperations operations;
    operations.write = [&](int descriptor, const void* bytes, std::size_t) -> ssize_t {
        if (++calls == 1) { errno = EINTR; return -1; }
        return ::write(descriptor, bytes, 1);
    };
    REQUIRE_FALSE(cli::save_atomic(directory.path / "result", {"0\n\n", 1, SaveKind::initial}, operations));
    REQUIRE(directory.read() == "0\n\n");
}
