/*
 *  FDOmenu - Menu code generator for icewm
 *  Copyright (C) 2015-2024 Eduard Bloch
 *
 *  Inspired by icewm-menu-gnome2 and Freedesktop.org specifications
 *  Using pure glib/gio code and a built-in menu structure instead
 *  the XML based external definition (as suggested by FD.o specs)
 *
 *  Released under terms of the GNU Library General Public License
 *  (version 2.0)
 *
 *  2015/02/05: Eduard Bloch <edi@gmx.de>
 *  - initial version
 *  2018/08:
 *  - overhauled program design and menu construction code, added sub-category
 * handling
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "base.h"
#include "config.h"
// #include "sysdep.h"
#include "appnames.h"
#include "intl.h"
#include "ylocale.h"

#include <cstring>
#include <stack>
#include <string>
// does not matter, string from C++11 is enough
// #include <string_view>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

using namespace std;

#include "fdospecgen.h"

char const *ApplicationName;

#ifdef DEBUG
#define DBGMSG(x) cerr << x << endl;
#else
#define DBGMSG(x)
#endif

// program options
bool add_sep_before = false;
bool add_sep_after = false;
bool no_sep_others = false;
bool no_sub_cats = false;
bool generic_name = false;
bool right_to_left = false;
bool flat_output = false;
bool match_in_section = false;
bool match_in_section_only = false;

auto substr_filter = "";
auto substr_filter_nocase = "";
auto flat_sep = " / ";
char *terminal_command;
char *terminal_option;

/*
 * Certain parts borrowed from apt-cacher-ng by its autor, either from older
 * branches (C++11 compatible) or development branch.
 */

#define startsWith(where, what) (0 == (where).compare(0, (what).size(), (what)))
#define startsWithSz(where, what)                                              \
    (0 == (where).compare(0, sizeof((what)) - 1, (what)))
#define endsWith(where, what)                                                  \
    ((where).size() >= (what).size() &&                                        \
     0 == (where).compare((where).size() - (what).size(), (what).size(),       \
                          (what)))
#define endsWithSzAr(where, what)                                              \
    ((where).size() >= (sizeof((what)) - 1) &&                                 \
     0 == (where).compare((where).size() - (sizeof((what)) - 1),               \
                          (sizeof((what)) - 1), (what)))

/**
 * Basic base implementation of a reference-counted class
 */
struct tLintRefcounted {
  private:
    size_t m_nRefCount = 0;

  public:
    inline void __inc_ref() noexcept { m_nRefCount++; }
    inline void __dec_ref() {
        if (--m_nRefCount == 0)
            delete this;
    }
    virtual ~tLintRefcounted() {}
    inline size_t __ref_cnt() { return m_nRefCount; }
};

/**
 * Lightweight intrusive smart pointer with ordinary reference counting
 */
template <class T> class lint_ptr {
    T *m_ptr = nullptr;

  public:
    explicit lint_ptr() {}
    /**
     * @brief lint_ptr Captures the pointer and ensures that it's released when
     * the refcount goes to zero, unless initialyTakeRef is set to false. If
     * initialyTakeRef is false, the operation is asymmetric, i.e. one extra
     * __dec_ref operation will happen in the end.
     *
     * @param rawPtr
     * @param initialyTakeRef
     */
    explicit lint_ptr(T *rawPtr, bool initialyTakeRef = true) : m_ptr(rawPtr) {
        if (rawPtr && initialyTakeRef)
            rawPtr->__inc_ref();
    }
    T *construct() {
        reset(new T);
        return m_ptr;
    }
    lint_ptr(const ::lint_ptr<T> &orig) : m_ptr(orig.m_ptr) {
        if (!m_ptr)
            return;
        m_ptr->__inc_ref();
    }
    lint_ptr(::lint_ptr<T> &&orig) {
        if (this == &orig)
            return;
        m_ptr = orig.m_ptr;
        orig.m_ptr = nullptr;
    }
    inline ~lint_ptr() {
        if (!m_ptr)
            return;
        m_ptr->__dec_ref();
    }
    T *get() const { return m_ptr; }
    bool operator==(const T *raw) const { return get() == raw; }
    inline void reset(T *rawPtr) noexcept {
        if (rawPtr == m_ptr) // heh?
            return;
        reset();
        m_ptr = rawPtr;
        if (rawPtr)
            rawPtr->__inc_ref();
    }
    inline void swap(lint_ptr<T> &other) { std::swap(m_ptr, other.m_ptr); }
    inline void reset() noexcept {
        if (m_ptr)
            m_ptr->__dec_ref();
        m_ptr = nullptr;
    }
    lint_ptr<T> &operator=(const lint_ptr<T> &other) {
        if (m_ptr == other.m_ptr)
            return *this;
        reset(other.m_ptr);
        return *this;
    }
    lint_ptr<T> &operator=(lint_ptr<T> &&other) {
        if (m_ptr == other.m_ptr)
            return *this;

        m_ptr = other.m_ptr;
        other.m_ptr = nullptr;
        return *this;
    }
    // pointer-like access options
    explicit inline operator bool() const noexcept { return m_ptr; }
    inline T &operator*() const noexcept { return *m_ptr; }
    inline T *operator->() const noexcept { return m_ptr; }
    // pointer-like access options
    inline bool operator<(const lint_ptr<T> &vs) const noexcept {
        return m_ptr < vs.m_ptr;
    }
    // pointer-like access options
    inline bool operator==(const lint_ptr<T> &vs) const noexcept {
        return m_ptr == vs.m_ptr;
    }
    /**
     * @brief release returns the pointer and makes this invalid while keeping
     * the refcount
     * @return Raw pointer
     */
    T *release() noexcept {
        auto ret = m_ptr;
        m_ptr = nullptr;
        return ret;
    }
};

/*!
 * \brief Simple and convenient split function, outputs resulting tokens into a
 * string vector. Operates on user-provided vector, with or without purging the
 * previous contents.
 */
vector<string> &Tokenize(const string &in, const char *sep,
                         vector<string> &inOutVec, bool bAppend = false,
                         std::string::size_type nStartOffset = 0) {
    if (!bAppend)
        inOutVec.clear();

    auto pos = nStartOffset, pos2 = nStartOffset, oob = in.length();
    while (pos < oob) {
        pos = in.find_first_not_of(sep, pos);
        if (pos == string::npos) // no more tokens
            break;
        pos2 = in.find_first_of(sep, pos);
        if (pos2 == string::npos) // no more terminators, EOL
            pos2 = oob;
        inOutVec.emplace_back(in.substr(pos, pos2 - pos));
        pos = pos2 + 1;
    }
    return inOutVec;
}

void replace_all(std::string &str, const std::string &from,
                 const std::string &to) {
    if (from.empty()) {
        return;
    }

    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

auto line_matcher = std::regex(
    "^\\s*(Terminal|Type|Name|Exec|TryExec|Icon|Categories|NoDisplay)("
    "\\[(..)\\])?\\s*=\\s*(.*){0,1}?\\s*$",
    std::regex_constants::ECMAScript);

struct DesktopFile : public tLintRefcounted {
    bool Terminal = false, IsApp = true, NoDisplay = false,
         CommandMassaged = false;
    string Name, NameLoc, Exec, TryExec, Icon;
    vector<string> Categories;

    /// Translate with built-in l10n if needed
    string &GetTranslatedName() {
        if (NameLoc.empty()) {
            NameLoc = gettext(Name.c_str());
        }
        return NameLoc;
    }

    const string &GetCommand() {

        if (CommandMassaged)
            return Exec;

        CommandMassaged = true;

        if (Terminal && terminal_command) {
            Exec = string(terminal_command) + " -e " + Exec;
        }

        // let's try whether the command line is toxic, expecting stuff from
        // https://specifications.freedesktop.org/desktop-entry-spec/latest/exec-variables.html
        if (string::npos == Exec.find('%'))
            return Exec;
        if (!TryExec.empty())
            return (Exec = TryExec); // copy over so we stick to it in case of
                                     // later calls

        for (const auto &bad : {"%F", "%U", "%f", "%u"})
            replace_all(Exec, bad, "");
        replace_all(Exec, "%c", Name);
        replace_all(Exec, "%i", Icon);

        return Exec;
    }

    DesktopFile(string filePath, const string &lang,
                const unordered_set<string> &allowed_names) {
        // cout << "filterlang: " << lang <<endl;
        std::ifstream dfile;
        dfile.open(filePath);
        string line;
        std::smatch m;
        bool reading = false;
        while (dfile) {
            line.clear();
            std::getline(dfile, line);
            if (line.empty()) {
                if (dfile.eof())
                    break;
                continue;
            }
            if (startsWithSz(line, "[Desktop ")) {
                if (startsWithSz(line, "[Desktop Entry")) {
                    reading = true;
                } else if (reading) // finished with desktop entry contents,
                                    // exit
                    break;
            }
            if (!reading)
                continue;

            std::regex_search(line, m, line_matcher);
            if (m.empty())
                continue;
            if (m[3].matched && m[3].compare(lang))
                continue;

            // for(auto x: m) cout << x << " - "; cout << " = " << m.size() <<
            // endl;

            if (m[1] == "Terminal")
                Terminal = m[4].compare("true") == 0;
            else if (m[1] == "NoDisplay")
                NoDisplay = m[4].compare("true") == 0;
            else if (m[1] == "Icon")
                Icon = m[4];
            else if (m[1] == "Categories") {
                Tokenize(m[4], ";", Categories);
            } else if (m[1] == "Exec") {
                Exec = m[4];
            } else if (m[1] == "TryExec") {
                TryExec = m[4];
            } else if (m[1] == "Type") {
                if (m[4] == "Application")
                    IsApp = true;
                else if (m[4] == "Directory")
                    IsApp = false;
                else
                    continue;
            } else { // must be name
                if (m[3].matched)
                    NameLoc = m[4];
                else {
                    Name = m[4];
                    // detect reading of a worthless desktop item ASAP
                    if (!allowed_names.empty() &&
                        allowed_names.find(Name) != allowed_names.end())
                        break;
                }
            }
        }
    }

    static lint_ptr<DesktopFile> load_visible(
        const string &path, const string &lang,
        const unordered_set<string> &wanted_names = unordered_set<string>()) {
        auto ret = lint_ptr<DesktopFile>();
        try {
            ret.reset(new DesktopFile(path, lang, wanted_names));
            if (ret->NoDisplay)
                ret.reset();
        } catch (const std::exception &) {
        }
        return ret;
    }
};

using DesktopFilePtr = lint_ptr<DesktopFile>;

template <typename T, typename C, C TFreeFunc(T)> struct auto_raii {
    T m_p;
    auto_raii(T xp) : m_p(xp) {}
    ~auto_raii() {
        if (m_p)
            TFreeFunc(m_p);
    }
};

const char *rtls[] = {
    "ar", // arabic
    "fa", // farsi
    "he", // hebrew
    "ps", // pashto
    "sd", // sindhi
    "ur", // urdu
};

struct tLessOp4Localized {
    std::locale loc; // default locale
    const std::collate<char> &coll = std::use_facet<std::collate<char>>(loc);
    bool operator()(const std::string &a, const std::string &b) {
        return coll.compare(a.data(), a.data() + a.size(), b.data(),
                            b.data() + b.size()) < 0;
    }
} locStringComper;

class FsScan {
  private:
    std::set<std::pair<ino_t, dev_t>> reclog;
    function<void(const string &)> cb;
    string sFileNameExtFilter;
    bool recursive;

  public:
    FsScan(decltype(FsScan::cb) cb, const string &sFileNameExtFilter = "",
           bool recursive = true)
        : cb(cb), sFileNameExtFilter(sFileNameExtFilter), recursive(recursive) {
    }
    void scan(const string &sStartdir) { proc_dir_rec(sStartdir); }

  private:
    void proc_dir_rec(const string &path) {

        DBGMSG("enter: " << path);

        auto pdir = opendir(path.c_str());
        if (!pdir)
            return;
        auto_raii<DIR *, int, closedir> dircloser(pdir);
        auto fddir(dirfd(pdir));

        // const gchar *szFilename(nullptr);
        dirent *pent;
        struct stat stbuf;

        while (nullptr != (pent = readdir(pdir))) {
            if (pent->d_name[0] == '.')
                continue;

            string fname(pent->d_name);

            if (fstatat(fddir, pent->d_name, &stbuf, 0))
                continue;

            if (recursive && S_ISDIR(stbuf.st_mode)) {
                // link loop detection
                auto prev = make_pair(stbuf.st_ino, stbuf.st_dev);
                auto hint = reclog.insert(prev);
                if (hint.second) { // we added a new one, otherwise do not
                                   // descend
                    proc_dir_rec(path + "/" + fname);
                    reclog.erase(hint.first);
                }
            }

            if (!S_ISREG(stbuf.st_mode))
                continue;

            if (!sFileNameExtFilter.empty() &&
                !endsWith(fname, sFileNameExtFilter)) {

                continue;
            }

            cb(path + "/" + fname);
        }
    }
};

static void help(bool to_stderr, int xit) {
    (to_stderr ? cerr : cout)
        << "USAGE: icewm-menu-fdo [OPTIONS] [FILENAME]\n"
           "OPTIONS:\n"
           "-g, --generic\tInclude GenericName in parentheses of progs\n"
           "-o, --output=FILE\tWrite the output to FILE\n"
           "-t, --terminal=NAME\tUse NAME for a terminal that has '-e'\n"
           "--seps  \tPrint separators before and after contents\n"
           "--sep-before\tPrint separator before the contents\n"
           "--sep-after\tPrint separator only after contents\n"
           "--no-sep-others\tLegacy, has no meaning\n"
           "--no-sub-cats\tNo additional subcategories, just one level of "
           "menues\n"
           "--flat\tDisplay all apps in one layer with category hints\n"
           "--flat-sep STR\tCategory separator string used in flat mode "
           "(default: ' / ')\n"
           "--match PAT\tDisplay only apps with title containing PAT\n"
           "--imatch PAT\tLike --match but ignores the letter case\n"
           "--match-sec\tApply --match or --imatch to apps AND sections\n"
           "--match-osec\tApply --match or --imatch only to sections\n"
           "FILENAME\tAny .desktop file to launch its application Exec "
           "command\n"
           "This program also listens to environment variables defined by\n"
           "the XDG Base Directory Specification:\n"
           "XDG_DATA_HOME="
        << Elvis(getenv("XDG_DATA_HOME"), (char *)"")
        << "\n"
           "XDG_DATA_DIRS="
        << Elvis(getenv("XDG_DATA_DIRS"), (char *)"") << endl;
    exit(xit);
}

/**
 * The own menu deco info is not part of this class.
 * It's fetched on-demand with a supplied resolver function.
 */
struct MenuNode {
    void sink_in(DesktopFilePtr df);

    void print(std::ostream &prt_strm);

    // void collect_menu_names(const function<void(const string&)> &callback);

    void fixup();

    map<string, MenuNode> submenues;

    // using a map instead of set+logics adds a minor memory overhead but allows
    // simple duplicate detection (adding user's version first)
    unordered_map<string, DesktopFilePtr> apps;
    // unordered_set<string> dont_add_mark;

    static unordered_multimap<string, decltype(submenues)::iterator>
        menu_nodes_by_name;
    DesktopFilePtr deco;
};

decltype(MenuNode::menu_nodes_by_name) MenuNode::menu_nodes_by_name;

int main(int argc, char **argv) {

    // basic framework and environment initialization
    ApplicationName = my_basename(argv[0]);

#ifdef CONFIG_I18N
    setlocale(LC_ALL, "");

    auto msglang = YLocale::getCheckedExplicitLocale(false);
    right_to_left =
        msglang && std::any_of(rtls, rtls + ACOUNT(rtls), [&](const char *rtl) {
            return rtl[0] == msglang[0] && rtl[1] == msglang[1];
        });
    bindtextdomain(PACKAGE, LOCDIR);
    textdomain(PACKAGE);
#endif

    vector<string> sharedirs;
    const char *p;
    auto pUserShare = getenv("XDG_DATA_HOME");
    if (pUserShare && *pUserShare)
        Tokenize(pUserShare, ":", sharedirs, true);
    else if (nullptr != (p = getenv("HOME")) && *p)
        sharedirs.push_back(string(p) + "/.local/share");

    // system dirs, either from environment or from static locations
    auto sysshare = getenv("XDG_DATA_DIRS");
    if (sysshare && !*sysshare)
        Tokenize(sysshare, ":", sharedirs, true);
    else
        sharedirs.push_back("/usr/local/share"),
            sharedirs.push_back("/usr/share");

    for (auto pArg = argv + 1; pArg < argv + argc; ++pArg) {
        if (is_version_switch(*pArg)) {
            cout << "icewm-menu-fdo " VERSION ", Copyright 2015-2024 Eduard "
                    "Bloch, 2017-2023 Bert Gijsbers."
                 << endl;
            exit(0);
        } else if (is_copying_switch(*pArg))
            print_copying_exit();
        else if (is_help_switch(*pArg))
            help(false, EXIT_SUCCESS);
        else if (is_long_switch(*pArg, "seps"))
            add_sep_before = add_sep_after = true;
        else if (is_long_switch(*pArg, "sep-before"))
            add_sep_before = true;
        else if (is_long_switch(*pArg, "sep-after"))
            add_sep_after = true;
        else if (is_long_switch(*pArg, "no-sep-others"))
            no_sep_others = true;
        else if (is_long_switch(*pArg, "no-sub-cats"))
            no_sub_cats = true;
        else if (is_long_switch(*pArg, "flat"))
            flat_output = no_sep_others = true;
        else if (is_long_switch(*pArg, "match-sec"))
            match_in_section = true;
        else if (is_long_switch(*pArg, "match-osec"))
            match_in_section = match_in_section_only = true;
        else if (is_switch(*pArg, "g", "generic-name"))
            generic_name = true;
        else {
            char *value = nullptr, *expand = nullptr;
            if (GetArgument(value, "o", "output", pArg, argv + argc)) {
                if (*value == '~')
                    value = expand = tilde_expansion(value);
                else if (*value == '$')
                    value = expand = dollar_expansion(value);
                if (nonempty(value)) {
                    if (freopen(value, "w", stdout) == nullptr)
                        fflush(stdout);
                }
                if (expand)
                    delete[] expand;
            } else if (GetArgument(value, "m", "match", pArg, argv + argc))
                substr_filter = value;
            else if (GetArgument(value, "M", "imatch", pArg, argv + argc))
                substr_filter_nocase = value;
            else if (GetArgument(value, "F", "flat-sep", pArg, argv + argc))
                flat_sep = value;
            else if (GetArgument(value, "t", "terminal", pArg, argv + argc))
                terminal_option = value;
            else // unknown option
                help(true, EXIT_FAILURE);
        }
    }

    auto shortLang = string(msglang ? msglang : "").substr(0, 2);

    const char *terminals[] = {terminal_option, getenv("TERMINAL"), TERM,
                               "urxvt",         "alacritty",        "roxterm",
                               "xterm"};
    for (auto term : terminals)
        if (term && (terminal_command = path_lookup(term)) != nullptr)
            break;

    MenuNode root;

    auto desktop_loader = FsScan(
        [&](const string &fPath) {
            DBGMSG("reading: " << fPath);
            auto df = DesktopFile::load_visible(fPath, shortLang);
            if (df)
                root.sink_in(df);
        },
        ".desktop");

    for (const auto &sdir : sharedirs) {
        DBGMSG("checkdir: " << sdir);
        desktop_loader.scan(sdir + "/applications");
    }

    root.fixup();

    /*
        unordered_set<string> filter;
        for(const auto& kv: root.menu_nodes_by_name)
            filter.insert(kv.first);
    */

    auto dir_loader = FsScan(
        [&](const string &fPath) {
            // XXX: Filter not working as intended, and probably pointless
            // anyway because of the alternative checks, see below
            auto df = DesktopFile::load_visible(fPath, shortLang /*, filter*/);
            if (!df)
                return;

            // get all menu nodes of that name
            auto rng = root.menu_nodes_by_name.equal_range(df->Name);
            for (auto it = rng.first; it != rng.second; ++it) {
                if (!it->second->second.deco)
                    it->second->second.deco = df;
            }
            // No menus of that name? Try using the plain filename, some
            // .directory files use the category as file name stem but differing
            // in the Name attribute
            if (rng.first == rng.second) {
                auto cpos = fPath.find_last_of("/");
                auto mcatName =
                    fPath.substr(cpos + 1, fPath.length() - cpos - 11);
                rng = root.menu_nodes_by_name.equal_range(mcatName);
                DBGMSG("altname: " << mcatName);

                for (auto it = rng.first; it != rng.second; ++it) {
                    if (!it->second->second.deco)
                        it->second->second.deco = df;
                }
            }
        },
        ".directory", false);

    for (const auto &sdir : sharedirs) {
        dir_loader.scan(sdir + "/desktop-directories");
    }

    if (add_sep_before)
        cout << "separator" << endl;

    root.print(cout);

    if (add_sep_after)
        cout << "separator" << endl;

    return EXIT_SUCCESS;
}

void MenuNode::sink_in(DesktopFilePtr pDf) {

    auto add_sub_menues = [&](const t_menu_path &mp) {
        MenuNode *cur = this;

        // work around the lack of reverse iterator, can fixed in C++14 with
        // std::rbegin() conversion
        if (!mp.size())
            return cur;
        for (auto it = mp.end() - 1;; --it) {

            /*
            #warning Insufficient, works only when the keywords have the
            "friendly" order

            auto wrong_one = cur->apps.find(pDf->Name);
            if (wrong_one != cur->apps.end() && wrong_one->second == pDf) {
                cur->apps.erase(wrong_one);
            }

            This gets overcomplicated. Could be solved by getting the menu paths
            for each keyword first, sorting them by length (descending), then
            adding the deepest first and marking parent nodes as "visited"
            (another hashset or similar) to not add there again later.

            But then again, it's probably easier to just add them wherever they
            appear and use fixup() later.

            */

            auto key = (*it && **it) ? *it : "Other";
            auto added = cur->submenues.emplace(key, MenuNode());
            if (added.second) {
                menu_nodes_by_name.insert({key, added.first});
            }
            cur = &added.first->second;

            // cerr << "adding submenu: " << key << endl;
            // cur = &cur->submenues[key];
            if (mp.begin() == it)
                break;
        }
        return cur;
    };

    for (const auto &cat : pDf->Categories) {
        // cerr << "where does it fit? " << cat << endl;
        t_menu_path refval = {cat.c_str()};
        static auto comper = [](const t_menu_path &a, const t_menu_path &b) {
            // cerr << "left: " << *a.begin() << " vs. right: " << *b.begin() <<
            // endl;
            return strcmp(*a.begin(), *b.begin()) < 0;
        };
        for (const auto &w : valid_paths) {
            // cerr << "try paths: " << (uintptr_t)&w << endl;

            // ignore deeper paths, fallback to the main cats only
            if (no_sub_cats && w.begin()->size() > 1)
                continue;

            auto rng = std::equal_range(w.begin(), w.end(), refval, comper);
            for (auto it = rng.first; it != rng.second; ++it) {
                auto &tgt = *add_sub_menues(*it);
                tgt.apps.emplace(pDf->Name, pDf);
            }
        }
    }
}

static string ICON_FOLDER("folder");
string indent_hint("");

void MenuNode::print(std::ostream &prt_strm) {
    // translated name to icon and submenu (sorted by translated)
    map<string, std::pair<string, MenuNode *>, tLessOp4Localized> sorted;
    for (auto &m : this->submenues) {
        auto &name = m.first;
        auto &deco = m.second.deco;
        sorted[deco ? deco->GetTranslatedName() : name] =
            make_pair(deco ? deco->Icon : ICON_FOLDER, &m.second);
    }
    for (auto &m : sorted) {
        auto &name = m.first;
        prt_strm << indent_hint << "menu \""
                 << (m.second.second->deco
                         ? m.second.second->deco->GetTranslatedName()
                         : name)
                 << "\" " <<

            ((m.second.second->deco && !m.second.second->deco->Icon.empty())
                 ? m.second.second->deco->Icon
                 : ICON_FOLDER)
                 << " {\n";

        indent_hint += "\t";
        m.second.second->print(prt_strm);
        indent_hint.pop_back();

        prt_strm << indent_hint << "}\n";
    }
    map<string, DesktopFilePtr, tLessOp4Localized> sortedApps;
    for (auto &p : this->apps)
        sortedApps[p.second->GetTranslatedName()] = p.second;

    for (auto &p : sortedApps) {
        auto &pi = p.second;
        prt_strm << indent_hint << "prog \"" << pi->GetTranslatedName() << "\" "
                 << pi->Icon << " " << pi->GetCommand() << "\n";
    }
}

void MenuNode::fixup() {

    // descend deep and then check whether the same app has been added somewhere
    // in the parent nodes, then remove it there
    vector<MenuNode *> checkStack;
    std::function<void(MenuNode *)> go_deeper;
    go_deeper = [&](MenuNode *cur) {
        checkStack.push_back(cur);

        for (auto &sub : cur->submenues)
            go_deeper(&sub.second);

        for (auto &appIt : cur->apps) {
            for (auto ancestorIt = checkStack.begin();
                 ancestorIt != checkStack.end() - 1; ++ancestorIt) {
                auto otherIt = (*ancestorIt)->apps.find(appIt.second->Name);
                if (otherIt != (*ancestorIt)->apps.end() &&
                    otherIt->second == appIt.second) {
                    (*ancestorIt)->apps.erase(otherIt);
                }
            }
        }

        checkStack.pop_back();
    };
    go_deeper(this);
}

// vim: set sw=4 ts=4 et:
