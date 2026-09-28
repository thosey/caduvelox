#include <gtest/gtest.h>
#include "caduvelox/Server.hpp"
#include "caduvelox/jobs/IoJob.hpp"
#include "caduvelox/jobs/WriteJob.hpp"
#include "caduvelox/logger/ConsoleLogger.hpp"
#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace caduvelox;
namespace fs = std::filesystem;

/**
 * WriteJob against a regular file (review item L12).
 *
 * WriteJob documents itself as "writing data to a file descriptor", and
 * io_uring_prep_write has pwrite(2) semantics: the offset argument is where the
 * write lands. It passed 0. For a socket that is ignored, which is all this
 * framework uses it for -- but for a regular file every write went to the start,
 * so two writes overwrote each other and the resubmission after a partial write
 * would re-send over what it had just written.
 *
 * Offset -1 means "use and advance the file position", i.e. write(2).
 */
namespace {

class WriteJobFileOffsetTest : public ::testing::Test {
protected:
    void SetUp() override {
        static ConsoleLogger console_logger;
        Logger::setGlobalLogger(&console_logger);
        ASSERT_TRUE(server_.init(64));
        dir_ = fs::temp_directory_path() / ("cadu_l12_" + std::to_string(::getpid()));
        fs::create_directories(dir_);
        path_ = (dir_ / "out.bin").string();
        fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd_, 0);
    }

    void TearDown() override {
        if (fd_ >= 0) ::close(fd_);
        fs::remove_all(dir_);
    }

    // Run one WriteJob to completion.
    void writeOnce(const std::string& data) {
        done_ = false;
        auto* job = WriteJob::createFromPoolFromString(
            fd_, data,
            [this](int, size_t n) { written_ += n; done_ = true; },
            [this](int, int err) { error_ = err; done_ = true; });
        ASSERT_NE(job, nullptr);
        job->start(server_);

        while (!done_) {
            struct __kernel_timespec ts{};
            ts.tv_sec = 3;
            struct io_uring_cqe* cqe = nullptr;
            ASSERT_EQ(io_uring_wait_cqe_timeout(server_.getRing(), &cqe, &ts), 0)
                << "no completion for the write";
            auto* completed = reinterpret_cast<IoJob*>(io_uring_cqe_get_data64(cqe));
            auto cleanup = completed->handleCompletion(server_, cqe);
            io_uring_cqe_seen(server_.getRing(), cqe);
            if (cleanup) (*cleanup)(completed);
        }
        ASSERT_EQ(error_, 0) << "write failed with " << error_;
    }

    std::string fileContents() {
        std::ifstream f(path_, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    Server server_;
    fs::path dir_;
    std::string path_;
    int fd_ = -1;
    bool done_ = false;
    size_t written_ = 0;
    int error_ = 0;
};

TEST_F(WriteJobFileOffsetTest, SuccessiveWritesAppendRatherThanOverwrite) {
    writeOnce("one");
    writeOnce("two");

    EXPECT_EQ(fileContents(), "onetwo")
        << "each write landed at offset 0, so the second overwrote the first -- "
           "io_uring_prep_write has pwrite semantics and was being given 0";
    EXPECT_EQ(written_, 6u);
}

TEST_F(WriteJobFileOffsetTest, ASingleWriteStillLandsAtTheStart) {
    writeOnce("hello");
    EXPECT_EQ(fileContents(), "hello");
}

}  // namespace
