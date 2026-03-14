#pragma once

#include "clspc/lsp_types.h"

#include <filesystem>
#include <memory>
#include <string>

#include <pcr/proc/piped_child.h>


namespace clspc {

struct SessionOptions 
{
    std::filesystem::path root_dir;
    std::string client_name{"clspc"};
    std::string client_version{"0.1"};
};

class Session 
{
public:
    Session(pcr::proc::PipedChild child, SessionOptions options);

    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;
    ~Session();

    Session(const Session&) = delete;
    Session &operator=(const Session&) = delete;

    InitializeResult initialize();
    void initialized();
    void shutdown_and_exit();

    void wait();

    // document sync
    int sync_disk_file(const std::filesystem::path &path);
    int sync_text(const std::filesystem::path &path,
                  std::string text,
                  std::string language_id = "plaintext");
    void close_file(const std::filesystem::path &path);


private:

    // PIMPL idiom - avoid including a lot of pcr libs headers
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clspc
