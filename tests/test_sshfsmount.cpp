/*
 * Copyright (C) 2018-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "mock_logger.h"
#include "sftp_server_test_fixture.h"
#include "signal.h"

#include <multipass/exceptions/sshfs_missing_error.h>
#include <multipass/logging/log.h>
#include <multipass/optional.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/sshfs_mount/sshfs_mount.h>
#include <multipass/utils.h>

#include "extra_assertions.h"
#include <algorithm>
#include <gmock/gmock.h>
#include <iterator>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpt = multipass::test;

using namespace testing;

typedef std::vector<std::pair<std::string, std::string>> CommandVector;

namespace
{
struct SshfsMount : public mp::test::SftpServerTest
{
    SshfsMount()
    {
        mpl::set_logger(logger);
        channel_read.returnValue(0);
        channel_is_closed.returnValue(0);
    }

    ~SshfsMount()
    {
        mpl::set_logger(nullptr);
    }

    mp::SshfsMount make_sshfsmount(mp::optional<std::string> target = mp::nullopt)
    {
        mp::SSHSession session{"a", 42};
        return {std::move(session), default_source, target.value_or(default_target), default_map, default_map};
    }

    auto make_exec_that_fails_for(const std::vector<std::string>& expected_cmds, bool& invoked)
    {
        auto request_exec = [this, expected_cmds, &invoked](ssh_channel, const char* raw_cmd) {
            std::string cmd{raw_cmd};

            for (const auto expected_cmd : expected_cmds)
            {
                if (cmd.find(expected_cmd) != std::string::npos)
                {
                    invoked = true;
                    exit_status_mock.return_exit_code(SSH_ERROR);
                }
            }
            return SSH_OK;
        };
        return request_exec;
    }

    // The 'invoked' parameter binds the execution and read mocks. We need a better mechanism to make them
    // cooperate better, i.e., make the reader read only when a command was issued.
    auto make_exec_to_check_commands(const CommandVector& commands, std::string::size_type& remaining,
                                     CommandVector::const_iterator& next_expected_cmd, std::string& output,
                                     bool& invoked, mp::optional<std::string>& fail_cmd, mp::optional<bool>& fail_bool)
    {
        *fail_bool = false;

        auto request_exec = [this, &commands, &remaining, &next_expected_cmd, &output, &invoked, &fail_cmd,
                             &fail_bool](ssh_channel, const char* raw_cmd) {
            invoked = false;

            std::string cmd{raw_cmd};

            if (fail_cmd && cmd.find(*fail_cmd) != std::string::npos)
            {
                if (fail_bool)
                {
                    *fail_bool = true;
                }
                exit_status_mock.return_exit_code(SSH_ERROR);
            }
            else if (next_expected_cmd != commands.end())
            {
                // Check if the first element of the given commands list is what we are expecting. In that case,
                // give the correct answer. If not, check the rest of the list to see if we broke the execution
                // order.
                auto pred = [&cmd](auto it) { return cmd == it.first; };
                CommandVector::const_iterator found_cmd = std::find_if(next_expected_cmd, commands.end(), pred);

                if (found_cmd == next_expected_cmd)
                {
                    invoked = true;
                    output = next_expected_cmd->second;
                    remaining = output.size();
                    ++next_expected_cmd;

                    return SSH_OK;
                }
                else if (found_cmd != commands.end())
                {
                    output = found_cmd->second;
                    remaining = output.size();
                    ADD_FAILURE() << "\"" << (found_cmd->first) << "\" executed out of order; expected \""
                                  << next_expected_cmd->first << "\"";

                    return SSH_OK;
                }
            }

            // If the command list was entirely checked or if the executed command is not on the list, check the
            // default commands to see if there is an answer to the current command.
            auto it = default_cmds.find(cmd);
            if (it != default_cmds.end())
            {
                output = it->second;
                remaining = output.size();
                invoked = true;
            }

            return SSH_OK;
        };

        return request_exec;
    }

    auto make_channel_read_return(const std::string& output, std::string::size_type& remaining, bool& prereq_invoked)
    {
        auto channel_read = [&output, &remaining, &prereq_invoked](ssh_channel, void* dest, uint32_t count,
                                                                   int is_stderr, int) {
            if (!prereq_invoked)
                return 0u;
            const auto num_to_copy = std::min(count, static_cast<uint32_t>(remaining));
            const auto begin = output.begin() + output.size() - remaining;
            std::copy_n(begin, num_to_copy, reinterpret_cast<char*>(dest));
            remaining -= num_to_copy;
            return num_to_copy;
        };
        return channel_read;
    }

    void test_command_execution(const CommandVector& commands, mp::optional<std::string> target = mp::nullopt,
                                mp::optional<std::string> fail_cmd = mp::nullopt,
                                mp::optional<bool> fail_bool = mp::nullopt)
    {
        bool invoked{false};
        std::string output;
        auto remaining = output.size();
        CommandVector::const_iterator next_expected_cmd = commands.begin();

        auto channel_read = make_channel_read_return(output, remaining, invoked);
        REPLACE(ssh_channel_read_timeout, channel_read);

        auto request_exec =
            make_exec_to_check_commands(commands, remaining, next_expected_cmd, output, invoked, fail_cmd, fail_bool);
        REPLACE(ssh_channel_request_exec, request_exec);

        make_sshfsmount(target.value_or(default_target));

        EXPECT_TRUE(next_expected_cmd == commands.end()) << "\"" << next_expected_cmd->first << "\" not executed";
    }

    template <typename Matcher>
    auto make_cstring_matcher(const Matcher& matcher)
    {
        return Property(&mpl::CString::c_str, matcher);
    }

    mpt::ExitStatusMock exit_status_mock;
    decltype(MOCK(ssh_channel_read_timeout)) channel_read{MOCK(ssh_channel_read_timeout)};
    decltype(MOCK(ssh_channel_is_closed)) channel_is_closed{MOCK(ssh_channel_is_closed)};

    std::string default_source{"source"};
    std::string default_target{"target"};
    std::unordered_map<int, int> default_map;
    int default_id{1000};
    std::shared_ptr<NiceMock<mpt::MockLogger>> logger = std::make_shared<NiceMock<mpt::MockLogger>>();

    const std::unordered_map<std::string, std::string> default_cmds{
        {"snap run multipass-sshfs.env", "LD_LIBRARY_PATH=/foo/bar\nSNAP=/baz\n"},
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: 3.0.0\n"},
        {"pwd", "/home/ubuntu\n"},
        {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
         "/home/ubuntu/\n"},
        {"id -u", "1000\n"},
        {"id -g", "1000\n"},
        {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -o slave -o transform_symlinks -o allow_other :\"source\" "
         "\"target\"",
         "don't care\n"}};
};

// Mocks an incorrect return from a given command.
struct SshfsMountFail : public SshfsMount, public testing::WithParamInterface<std::string>
{
};

// Mocks correct execution of a given vector of commands/answers. The first string specifies the parameter with
// which make_sshfsmount mock is called.
struct SshfsMountExecute : public SshfsMount, public testing::WithParamInterface<std::pair<std::string, CommandVector>>
{
};

struct SshfsMountExecuteAndFail
    : public SshfsMount,
      public testing::WithParamInterface<std::tuple<std::string, CommandVector, std::string>>
{
};

// Mocks the server raising a std::invalid_argument.
struct SshfsMountExecuteThrowInvArg : public SshfsMount, public testing::WithParamInterface<CommandVector>
{
};

// Mocks the server raising a std::runtime_error.
struct SshfsMountExecuteThrowRuntErr : public SshfsMount, public testing::WithParamInterface<CommandVector>
{
};

} // namespace

//
// Define some parameterized test fixtures.
//

TEST_P(SshfsMountFail, test_failed_invocation)
{
    bool invoked_cmd{false};
    std::string output;
    std::string::size_type remaining;

    auto channel_read = make_channel_read_return(output, remaining, invoked_cmd);
    REPLACE(ssh_channel_read_timeout, channel_read);

    CommandVector empty;
    CommandVector::const_iterator it = empty.end();
    mp::optional<std::string> fail_cmd = mp::make_optional(GetParam());
    mp::optional<bool> invoked_fail = mp::make_optional(false);
    auto request_exec = make_exec_to_check_commands(empty, remaining, it, output, invoked_cmd, fail_cmd, invoked_fail);
    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), std::runtime_error);
    EXPECT_TRUE(*invoked_fail);
}

TEST_P(SshfsMountExecute, test_succesful_invocation)
{
    std::string target = GetParam().first;
    CommandVector commands = GetParam().second;

    test_command_execution(commands, target);
}

TEST_P(SshfsMountExecuteAndFail, test_succesful_invocation_and_fail)
{
    std::string target = std::get<0>(GetParam());
    CommandVector commands = std::get<1>(GetParam());
    std::string fail_command = std::get<2>(GetParam());

    ASSERT_NO_THROW(test_command_execution(commands, target, fail_command));
}

TEST_P(SshfsMountExecuteThrowInvArg, test_invalid_arg_when_executing)
{
    EXPECT_THROW(test_command_execution(GetParam()), std::invalid_argument);
}

TEST_P(SshfsMountExecuteThrowRuntErr, test_runtime_error_when_executing)
{
    EXPECT_THROW(test_command_execution(GetParam()), std::runtime_error);
}

//
// Instantiate the parameterized tests suites from above.
//

INSTANTIATE_TEST_SUITE_P(SshfsMountThrowWhenError, SshfsMountFail,
                         testing::Values("mkdir", "chown", "id -u", "id -g", "cd", "pwd"));

// Commands to check that a version of FUSE smaller that 3 gives a correct answer.
CommandVector old_fuse_cmds = {{"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: 2.9.0\n"},
                               {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -o slave -o transform_symlinks -o "
                                "allow_other -o nonempty :\"source\" \"target\"",
                                "don't care\n"}};

// Commands to check that a version of FUSE at least 3.0.0 gives a correct answer.
CommandVector new_fuse_cmds = {{"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: 3.0.0\n"},
                               {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -o slave -o transform_symlinks -o "
                                "allow_other :\"source\" \"target\"",
                                "don't care\n"}};

// Commands to check that an unknown version of FUSE gives a correct answer.
CommandVector unk_fuse_cmds = {{"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "weird fuse version\n"},
                               {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -o slave -o transform_symlinks -o "
                                "allow_other :\"source\" \"target\"",
                                "don't care\n"}};

// Commands to check that the server correctly creates the mount target.
CommandVector exec_cmds = {
    {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
     "/home/ubuntu/\n"},
    {"sudo /bin/bash -c 'cd \"/home/ubuntu/\" && mkdir -p \"target\"'", "\n"},
    {"sudo /bin/bash -c 'cd \"/home/ubuntu/\" && chown -R 1000:1000 \"target\"'", "\n"}};

// Commands to check that the server works if an absolute path is given.
CommandVector absolute_cmds = {
    {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
     "/home/ubuntu/\n"},
    {"sudo /bin/bash -c 'cd \"/home/ubuntu/\" && mkdir -p \"target\"'", "\n"},
    {"sudo /bin/bash -c 'cd \"/home/ubuntu/\" && chown -R 1000:1000 \"target\"'", "\n"}};

// Commands to check that it works for a nonexisting path.
CommandVector nonexisting_path_cmds = {
    {"sudo /bin/bash -c 'P=\"/nonexisting/path\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'", "/\n"},
    {"sudo /bin/bash -c 'cd \"/\" && mkdir -p \"nonexisting/path\"'", "\n"},
    {"sudo /bin/bash -c 'cd \"/\" && chown -R 1000:1000 \"nonexisting\"'", "\n"}};

// Check the execution of the CommandVector's above.
INSTANTIATE_TEST_SUITE_P(SshfsMountSuccess, SshfsMountExecute,
                         testing::Values(std::make_pair("target", old_fuse_cmds), std::make_pair("target", exec_cmds),
                                         std::make_pair("target", new_fuse_cmds), std::make_pair("target", exec_cmds),
                                         std::make_pair("target", unk_fuse_cmds), std::make_pair("target", exec_cmds),
                                         std::make_pair("/home/ubuntu/target", absolute_cmds),
                                         std::make_pair("/nonexisting/path", nonexisting_path_cmds)));

// Commands to test that when a mount path already exists, no mkdir nor chown is ran.
CommandVector execute_no_mkdir_cmds = {
    {"sudo /bin/bash -c 'P=\"/home/ubuntu/target\"; while [ ! -d \"$P/\" ]; do P=${P%/*}; done; echo $P/'",
     "/home/ubuntu/target/\n"}};

INSTANTIATE_TEST_SUITE_P(SshfsMountSuccessAndAvoidCommands, SshfsMountExecuteAndFail,
                         testing::Values(std::make_tuple("target", execute_no_mkdir_cmds, "mkdir"),
                                         std::make_tuple("target", execute_no_mkdir_cmds, "chown")));

// Check that some commands throw some exceptions.
CommandVector non_int_uid_cmds = {{"id -u", "1000\n"}, {"id -u", "ubuntu\n"}};
CommandVector non_int_gid_cmds = {{"id -g", "1000\n"}, {"id -g", "ubuntu\n"}};
CommandVector invalid_fuse_ver_cmds = {
    {"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version: fu.man.chu\n"}};

INSTANTIATE_TEST_SUITE_P(SshfsMountThrowInvArg, SshfsMountExecuteThrowInvArg,
                         testing::Values(non_int_uid_cmds, non_int_gid_cmds));

INSTANTIATE_TEST_SUITE_P(SshfsMountThrowRuntErr, SshfsMountExecuteThrowRuntErr, testing::Values(invalid_fuse_ver_cmds));

//
// Finally, individual test fixtures.
//

TEST_F(SshfsMount, throws_when_sshfs_does_not_exist)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"sudo multipass-sshfs.env", "which sshfs"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    EXPECT_THROW(make_sshfsmount(), mp::SSHFSMissingError);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, unblocks_when_sftpserver_exits)
{
    mpt::Signal client_message;
    auto get_client_msg = [&client_message](sftp_session) {
        client_message.wait();
        return nullptr;
    };
    REPLACE(sftp_get_client_message, get_client_msg);

    bool stopped_ok = false;
    std::thread mount([&] {
        test_command_execution(CommandVector());
        stopped_ok = true;
    });

    client_message.signal();

    mount.join();
    EXPECT_TRUE(stopped_ok);
}

TEST_F(SshfsMount, blank_fuse_version_logs_error)
{
    CommandVector commands = {{"sudo env LD_LIBRARY_PATH=/foo/bar /baz/bin/sshfs -V", "FUSE library version:\n"}};

    EXPECT_CALL(*logger, log(Matcher<multipass::logging::Level>(_), Matcher<multipass::logging::CString>(_),
                             Matcher<multipass::logging::CString>(_)))
        .WillRepeatedly(Return());
    EXPECT_CALL(*logger, log(Eq(mpl::Level::warning), make_cstring_matcher(StrEq("sshfs mount")),
                             make_cstring_matcher(StrEq("Unable to parse the FUSE library version"))));
    EXPECT_CALL(*logger,
                log(Eq(mpl::Level::debug), make_cstring_matcher(StrEq("sshfs mount")),
                    make_cstring_matcher(StrEq("Unable to parse the FUSE library version: FUSE library version:"))));

    test_command_execution(commands);
}

TEST_F(SshfsMount, throws_install_sshfs_which_snap_fails)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"which snap"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    mp::SSHSession session{"a", 42};

    EXPECT_THROW(mp::utils::install_sshfs_for("foo", session), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_install_sshfs_no_snap_dir_fails)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"[ -e /snap ]"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    mp::SSHSession session{"a", 42};

    EXPECT_THROW(mp::utils::install_sshfs_for("foo", session), std::runtime_error);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, throws_install_sshfs_snap_install_fails)
{
    bool invoked{false};
    auto request_exec = make_exec_that_fails_for({"sudo snap install multipass-sshfs"}, invoked);
    REPLACE(ssh_channel_request_exec, request_exec);

    mp::SSHSession session{"a", 42};

    EXPECT_THROW(mp::utils::install_sshfs_for("foo", session), mp::SSHFSMissingError);
    EXPECT_TRUE(invoked);
}

TEST_F(SshfsMount, install_sshfs_no_failures_does_not_throw)
{
    mp::SSHSession session{"a", 42};

    EXPECT_NO_THROW(mp::utils::install_sshfs_for("foo", session));
}

TEST_F(SshfsMount, install_sshfs_timeout_logs_info)
{
    ssh_channel_callbacks callbacks{nullptr};
    bool sleep{false};

    auto request_exec = [&sleep](ssh_channel, const char* raw_cmd) {
        std::string cmd{raw_cmd};
        if (cmd == "sudo snap install multipass-sshfs")
            sleep = true;

        return SSH_OK;
    };
    REPLACE(ssh_channel_request_exec, request_exec);

    auto add_channel_cbs = [&callbacks](ssh_channel, ssh_channel_callbacks cb) mutable {
        callbacks = cb;
        return SSH_OK;
    };
    REPLACE(ssh_add_channel_callbacks, add_channel_cbs);

    auto event_dopoll = [&callbacks, &sleep](ssh_event, int timeout) {
        if (!callbacks)
            return SSH_ERROR;

        if (sleep)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout + 1));
        else
            callbacks->channel_exit_status_function(nullptr, nullptr, 0, callbacks->userdata);

        return SSH_OK;
    };
    REPLACE(ssh_event_dopoll, event_dopoll);

    EXPECT_CALL(*logger, log(Matcher<multipass::logging::Level>(_), Matcher<multipass::logging::CString>(_),
                             Matcher<multipass::logging::CString>(_)))
        .WillRepeatedly(Return());
    EXPECT_CALL(*logger, log(Eq(mpl::Level::info), make_cstring_matcher(StrEq("utils")),
                             make_cstring_matcher(StrEq("Timeout while installing 'sshfs' in 'foo'"))));

    mp::SSHSession session{"a", 42};

    mp::utils::install_sshfs_for("foo", session, std::chrono::milliseconds(1));
}
