#include <sys/inotify.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>

#include <iostream>
#include <cstring>
#include <cerrno>
#include <vector>
#include <functional>
#include <getopt.h>

#include "config.h"

constexpr bool debug = DEBUG;

void error(int rc, const char *msg)
{
    std::cerr << "autorun: " << msg << ": " << std::strerror(rc) << '\n';
}

void error(int rc, const std::string& msg)
{
    error(rc, msg.c_str());
}

class inotify {
    public:
        inotify() : _watches{}, _infd{}
        {
            _infd = inotify_init1(0);
        }

        bool add_watch(const char *filename)
        {
            int wd = inotify_add_watch(_infd, filename,
                                       IN_MOVE | IN_MODIFY| IN_CREATE | IN_DELETE);
            _watches.push_back(wd);
            return wd >= 0;
        }

        int fd()
        {
            return _infd;
        }

        ~inotify()
        {
            for (auto wd: _watches)
                inotify_rm_watch(_infd, wd);

            if (close(_infd) == -1)
                error(errno, "close");
        }

    private:
        std::vector<int> _watches;
        int _infd;
};

class epoll {
    public:
        using on_event_t = std::function<bool(struct epoll_event *)>;

        epoll() : _efd{}
        {
            _efd = epoll_create1(0);
        }

        bool add(int fd)
        {
            struct epoll_event event;

            event.data.fd = fd;
            event.events = EPOLLIN | EPOLLOUT;

            int rc = epoll_ctl(_efd, EPOLL_CTL_ADD, fd, &event);
            if (rc)
                error(errno, "epoll_ctl");

            return rc == 0;
        }

        void wait(on_event_t cb)
        {
            struct epoll_event event[2];
            bool running = true;
            int rc = 0;

            while (running) {
                rc = epoll_wait(_efd, event, 2, -1);

                if (rc == -1 && errno != EINTR) {
                    error(errno, "epoll_wait");
                    return;
                } else if (errno == EINTR) {
                    continue;
                }

                running = cb(event);
            }
        }

    private:
        int _efd;
};

void traverse(FTS *iter, inotify& in)
{
    FTSENT *file;

    while ((file = fts_read(iter)) != nullptr) {
        if constexpr (debug) {
            std::clog << iter->fts_path << '\n';
        }
        bool res = in.add_watch(iter->fts_path);
        if (!res) {
            auto msg = std::string{"inotify::add_watch "};
            msg.append(iter->fts_path);
            error(errno, msg);
            return;
        }
    }

    if (errno)
        error(errno, "fts_read");
}

int run_cmd(const char *cmd)
{
    return system(cmd);
}

void version(const char *progname)
{
    std::clog << progname << " version " VERSION "\n";
}

void usage(const char *progname)
{
    std::clog << progname << R"( [--file|-f <filename>] [--dir|-d <dirname>] <cmd>

    --help|-h    display this message
    --version|-v current version
    --file|-f    name of the file whose events will trigger <cmd>
    --dir|-d     all events on files and directories inside <dirname> will trigger <cmd>
    <cmd>        the command that will be run when an event is detected)"
    << '\n';
}

constexpr struct option cmd_args[] = {
    { "dir",     required_argument, nullptr, 'd', },
    { "file",    required_argument, nullptr, 'f', },
    { "help",    no_argument,       nullptr, 'h', },
    { "version", no_argument,       nullptr, 'v', },
    { nullptr,   no_argument,       nullptr, '\0' },
};

struct cli_option {
    std::string basename;
    std::string cmd;
    bool is_dir = false;
};

cli_option parse_opt(int argc, char *argv[])
{
    int option_index, opt;
    cli_option cli;

    while ((opt = getopt_long(argc, argv, "d:f:hv", cmd_args, &option_index)) != -1) {
        switch (opt) {
            case 'd':
                cli.is_dir = true;
                [[fallthrough]];
            case 'f':
                cli.basename = optarg;
                break;
            case 'v':
                version(argv[0]);
                exit(0);
            case 'h':
                usage(argv[0]);
                exit(0);
            case '?':
            default:
                usage(argv[0]);
                exit(1);
        }
    }

    if (optind < argc) {
        char **iter = argv + optind;

        while (*iter) {
            cli.cmd.append(*iter);
            cli.cmd.push_back(' ');
            iter++;
        }
    }

    return cli;
}

const char *inotify_event2str(const inotify_event *event)
{
    if (event->mask & IN_ACCESS)
        return "IN_ACCESS";
    else if (event->mask & IN_ATTRIB)
        return "IN_ATTRIB";
    else if (event->mask & IN_CLOSE_WRITE)
        return "IN_CLOSE_WRITE";
    else if (event->mask & IN_CLOSE_NOWRITE)
        return "IN_CLOSE_NOWRITE";
    else if (event->mask & IN_CREATE)
        return "IN_CREATE";
    else if (event->mask & IN_DELETE)
        return "IN_DELETE";
    else if (event->mask & IN_DELETE_SELF)
        return "IN_DELETE_SELF";
    else if (event->mask & IN_MODIFY)
        return "IN_MODIFY";
    else if (event->mask & IN_MOVE_SELF)
        return "IN_MOVE_SELF";
    else if (event->mask & IN_MOVED_FROM)
        return "IN_MOVED_FROM";
    else if (event->mask & IN_MOVED_TO)
        return "IN_MOVED_TO";
    else if (event->mask & IN_OPEN)
        return "IN_OPEN";
    else if (event->mask & IN_IGNORED)
        return "IN_IGNORED";
    else if (event->mask & IN_ONLYDIR)
        return "IN_ONLYDIR";
    else if (event->mask & IN_DONT_FOLLOW)
        return "IN_DONT_FOLLOW";
    else if (event->mask & IN_EXCL_UNLINK)
        return "IN_EXCL_UNLINK";
    else if (event->mask & IN_MASK_ADD)
        return "IN_MASK_ADD";
    else if (event->mask & IN_ONESHOT)
        return "IN_ONESHOT";
    else
        return "Unknown";
}

bool on_event(inotify& in, cli_option& cli_opts, struct epoll_event *e)
{
    char buf[10 * sizeof(struct inotify_event)];
    struct inotify_event *event;

    if (e->data.fd != in.fd())
        return true;

    int rc = read(e->data.fd, buf, sizeof(buf) - 1);
    if constexpr (debug) {
        std::clog
            << "Read rc=" << rc
            << ", sizeof(struct inotify_event)=" << sizeof(struct inotify_event)
            << ", sizeof(buf)-1=" << sizeof(buf)-1 << '\n';
    }
    if (rc == -1)
        return false;

    event = reinterpret_cast<struct inotify_event *>(buf);
    if (event->mask & IN_IGNORED)
        /* FIXME wont work if ignored is sent in a dir */
        in.add_watch(cli_opts.basename.c_str());

    if constexpr (debug) {
        std::clog << "Event: " << inotify_event2str(event) << '\n';
        std::clog << "Event: " << event->mask << '\n';
    }

    /* XXX what to do with rc ? */
    rc = run_cmd(cli_opts.cmd.c_str());
    return true;
}

int watch_dir(const cli_option& cli_opts, inotify& in)
{
    char *const rootname[] = {
        strdup(cli_opts.basename.c_str()),
        nullptr
    };
    FTS *root;

    root = fts_open(rootname, FTS_PHYSICAL | FTS_NOSTAT | FTS_NOCHDIR, nullptr);
    if (!root) {
        error(errno, "fts_open");
        return -1;
    }

    traverse(root, in);
    fts_close(root);

    return 0;
}

int watch_file(const cli_option& cli_opts, inotify& in)
{
    return in.add_watch(cli_opts.basename.c_str()) ? 0 : -1;
}

void clear_screen()
{
    std::cout << "\033[2J\033[1;1H";
    std::cout.flush();
}

int main(int argc, char *argv[])
{
    auto cli_opts = parse_opt(argc, argv);
    inotify in;
    epoll ep;
    int rc;

    if (cli_opts.is_dir)
        rc = watch_dir(cli_opts, in);
    else
        rc = watch_file(cli_opts, in);

    if (rc)
        return errno;

    ep.add(in.fd());

    clear_screen();

    ep.wait([&in, &cli_opts](struct epoll_event *e) -> bool {
        clear_screen();
        return on_event(in, cli_opts, e);
    });

    return 0;
}
