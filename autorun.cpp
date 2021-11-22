#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/stat.h>
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
#include <map>

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

        bool add_watch(const std::string& filename)
        {
            return add_watch(filename.c_str());
        }

        bool add_watch(const char *filename)
        {
            int wd = inotify_add_watch(_infd, filename,
                                       IN_MOVE | IN_MODIFY| IN_CREATE | IN_DELETE);
            _watches[wd] = filename;
            return wd >= 0;
        }

        int fd()
        {
            return _infd;
        }

        auto get_file(int wd) -> const std::string&
        {
            return _watches[wd];
        }

        ~inotify()
        {
            for (auto wd: _watches)
                inotify_rm_watch(_infd, wd.first);

            if (close(_infd) == -1)
                error(errno, "close");
        }

    private:
        std::map<int, std::string> _watches;
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
        if constexpr (debug)
            std::clog << iter->fts_path << '\n';

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
    std::clog << progname << R"( [--file|-f <filenames>] [--dir|-d <dirnames>] <cmd>

    --help|-h    display this message
    --version|-v current version
    --file|-f    name of the files whose events will trigger <cmd>
    --dir|-d     all events on files and directories inside <dirnames> will trigger <cmd>
                 (autorun will watch . by default)
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
    std::vector<std::string> filenames;
    std::vector<std::string> dirnames;
    std::string cmd;
};

bool is_dir(const char *filename)
{
    struct stat st;

    if (stat(filename, &st) == 0 && S_ISDIR(st.st_mode))
        return true;
    else
        return false;
}

bool is_reg(const char *filename)
{
    struct stat st;

    if (stat(filename, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    else
        return false;
}

void add_dir(const char *dirname, std::vector<std::string>& dirnames)
{
    if (is_dir(dirname)) {
        dirnames.push_back(dirname);
    } else {
        if (errno)
            error(errno, dirname);
        else
            std::cerr << "autorun: " << optarg << " is not a directory.\n";

        exit(1);
    }
}

void add_file(const char *filename, std::vector<std::string>& filenames)
{
    if (is_reg(filename)) {
        filenames.push_back(filename);
    } else {
        if (errno)
            error(errno, filename);
        else
            std::cerr << "autorun: " << optarg << " is not a file.\n";

        exit(1);
    }
}

cli_option parse_opt(int argc, char *argv[])
{
    bool parse_dir = false;
    int option_index, opt;
    cli_option cli;

    while ((opt = getopt_long(argc, argv, "-f:-d:hv", cmd_args, &option_index)) != -1) {
        switch (opt) {
            case 'd':
                parse_dir = true;
                add_dir(optarg, cli.dirnames);
                break;
            case 'f':
                parse_dir = false;
                add_file(optarg, cli.filenames);
                break;
            case 1:
                if (parse_dir)
                    add_dir(optarg, cli.dirnames);
                else
                    add_file(optarg, cli.filenames);
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

    if (cli.dirnames.size() == 0 && cli.filenames.size() == 0)
        cli.dirnames.push_back(".");

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
            << "read: rc=" << rc
            << ", sizeof(struct inotify_event)=" << sizeof(struct inotify_event)
            << ", sizeof(buf)-1=" << sizeof(buf)-1 << '\n';
    }
    if (rc == -1)
        return false;

    event = reinterpret_cast<struct inotify_event *>(buf);

    if (event->mask & IN_IGNORED)
        in.add_watch(in.get_file(event->wd));

    if (event->mask & (IN_CREATE | IN_ISDIR))
        in.add_watch(in.get_file(event->wd) + '/' + event->name);

    if constexpr (debug) {
        std::clog << "Event: " << inotify_event2str(event) << '\n';
        std::clog << "Name: " << in.get_file(event->wd);
        if (event->mask & IN_ISDIR)
            std::clog << '/' << event->name;
        std::clog << '\n';
    }

    /* XXX what to do with rc ? */
    rc = run_cmd(cli_opts.cmd.c_str());
    return true;
}

int watch_dir(const cli_option& cli_opts, inotify& in)
{
    std::vector<char *> rootname;
    FTS *root;

    rootname.resize(cli_opts.dirnames.size() + 1);
    auto iter = rootname.begin();
    for (auto dir: cli_opts.dirnames)
        *iter++ = strdup(dir.c_str());
    *iter = nullptr;

    root = fts_open(&rootname[0], FTS_PHYSICAL | FTS_NOSTAT | FTS_NOCHDIR, nullptr);
    if (!root) {
        error(errno, "fts_open");
        return -1;
    }

    traverse(root, in);
    fts_close(root);

    for (auto dir: rootname)
        free(dir);

    return 0;
}

bool watch_file(const cli_option& cli_opts, inotify& in)
{
    for (auto f: cli_opts.filenames) {
        bool rc = in.add_watch(f.c_str());
        if (!rc)
            return false;
    }
    return true;
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
    int rc = 0;

    if (!cli_opts.dirnames.empty()) {
        rc = watch_dir(cli_opts, in);
        if (rc) {
            error(errno, "watch_dir");
            return errno;
        }

    }

    if (!cli_opts.filenames.empty()) {
        rc = watch_file(cli_opts, in);
        if (!rc) {
            error(errno, "watch_file");
            return errno;
        }
    }
    ep.add(in.fd());

    if constexpr (!debug)
        clear_screen();

    ep.wait([&in, &cli_opts](struct epoll_event *e) -> bool {
        if constexpr (!debug)
            clear_screen();
        return on_event(in, cli_opts, e);
    });

    return 0;
}
