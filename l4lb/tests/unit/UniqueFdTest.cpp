#include "Test.h"

#include "net/UniqueFd.h"

#include <cerrno>
#include <fcntl.h>
#include <utility>
#include <unistd.h>

using l4lb::net::UniqueFd;

L4LB_TEST(UniqueFdMovesOwnership) {
    int pipe_fds[2]{};
    L4LB_REQUIRE(::pipe(pipe_fds) == 0);
    UniqueFd read_end(pipe_fds[0]);
    UniqueFd write_end(pipe_fds[1]);

    UniqueFd moved(std::move(read_end));
    L4LB_REQUIRE(!read_end.IsValid());
    L4LB_REQUIRE(moved.Get() == pipe_fds[0]);

    write_end = std::move(moved);
    L4LB_REQUIRE(!moved.IsValid());
    L4LB_REQUIRE(write_end.Get() == pipe_fds[0]);
    L4LB_REQUIRE(::fcntl(pipe_fds[1], F_GETFD) == -1);
    L4LB_REQUIRE(errno == EBADF);
}

L4LB_TEST(UniqueFdReleaseAndResetAreExplicit) {
    int pipe_fds[2]{};
    L4LB_REQUIRE(::pipe(pipe_fds) == 0);
    UniqueFd read_end(pipe_fds[0]);
    UniqueFd write_end(pipe_fds[1]);

    const int released = read_end.Release();
    L4LB_REQUIRE(!read_end.IsValid());
    L4LB_REQUIRE(::fcntl(released, F_GETFD) != -1);
    ::close(released);

    write_end.Reset();
    L4LB_REQUIRE(!write_end.IsValid());
    L4LB_REQUIRE(::fcntl(pipe_fds[1], F_GETFD) == -1);
    L4LB_REQUIRE(errno == EBADF);
}

