#include <unordered_map>

#include "portotop.hpp"
#include "version.hpp"

static double ParseNumber(const std::string &str) {
    try {
        return stod(str);
    } catch (...) {
        return NAN;
    }
}

static double ParseValue(const std::string &value, bool map) {
    if (!map)
        return ParseNumber(value);

    double ret = 0;
    unsigned long start_v = 0;
    for (unsigned long off = 0; off < value.length(); off++) {
        if (value[off] == ':')
            start_v = off + 2; // key: value
        else if (value[off] == ';')
            ret += ParseNumber(value.substr(start_v, off - start_v));
    }
    return ret + ParseNumber(value.substr(start_v));
}

static double DfDt(double curr, double prev, uint64_t dt) {
    if (dt)
        return 1000.0 * (curr - prev) / dt;
    return 0;
}

static double PartOf(double value, double total) {
    return value / total;
}
////////////////////////////////////////////////////////////////////////////////

int TConsoleScreen::Width() {
    return getmaxx(Wnd);
}
int TConsoleScreen::Height() {
    return getmaxy(Wnd);
}

TConsoleScreen::TConsoleScreen() {
    Wnd = initscr();
    clear();
    cbreak();
    noecho();
    intrflush(stdscr, true);
    keypad(stdscr, true);
    curs_set(0);
}
TConsoleScreen::~TConsoleScreen() {
    endwin();
}
void TConsoleScreen::SetTimeout(int ms) {
    timeout(ms);
}
template<class T>
void TConsoleScreen::PrintAt(T arg, int x, int y, int width, bool leftaligned, int attr) {
    PrintAt(std::to_string(arg), x, y, width, leftaligned, attr);
}
void TConsoleScreen::PrintAt(std::string str0, int x0, int y0, int w0, bool leftaligned,
             int attr) {
    if (x0 + w0 < 0 || x0 >= Width())
        return;

    int x = x0 < 0 ? 0 : x0;
    int w = w0 - (x - x0);
    if (x + w >= Width())
        w = Width() - x;

    std::string str;
    if ((int)str0.length() > x - x0)
        str = str0.substr(x - x0, w);
    else
        str = std::string(w, ' ');

    if (attr)
        attron(attr);
    mvprintw(y0, x, (leftaligned ? "%-*s" : "%*s"), w, str.c_str());
    if (attr)
        attroff(attr);
}
void TConsoleScreen::Refresh() {
    refresh();
}
void TConsoleScreen::Erase() {
    erase();
}
void TConsoleScreen::Clear() {
    clear();
}
int TConsoleScreen::Getch() {
    return wgetch(Wnd);
}
void TConsoleScreen::Save() {
    def_prog_mode();
    endwin();
}
void TConsoleScreen::Restore() {
    pid_t pid = getpgrp();

    if (pid >= 0)
        tcsetpgrp(1, pid);

    reset_prog_mode();
    refresh();
}
int TConsoleScreen::Dialog(std::string text, const std::vector<std::string> &buttons) {
    int selected = 0;
    bool done = false;

    int x0 = Width() / 2 - text.length() / 2;
    int y0 = Height() / 2 - 3;

    int w = 0;
    for (auto &b : buttons)
        w += b.length() + 1;
    int x00 = Width() / 2 - w / 2;

    WINDOW *win = newwin(5, std::max((int)text.length(), w) + 4, y0 - 1, std::min(x0, x00) - 2);
    box(win, 0, 0);
    wrefresh(win);

    while (!done) {
        PrintAt(text, x0, y0, text.length(), false);

        int x = x00;
        int n = 0;
        for (auto &b : buttons) {
            PrintAt(b, x, y0 + 2, b.length(), false, selected == n ? A_REVERSE : 0);
            x += 1 + b.length();
            n++;
        }

        switch(Getch()) {
        case KEY_LEFT:
            if (--selected < 0)
                selected = 0;
            break;
        case KEY_RIGHT:
            if ((unsigned long) ++selected > buttons.size() - 1)
                selected = buttons.size() - 1;
            break;
        case '\n':
            done = true;
            break;
        }

        Refresh();
    }

    delwin(win);

    return selected;
}
void TConsoleScreen::ErrorDialog(Porto::Connection &api) {
    std::string message;
    int error;

    api.GetLastError(error, message);

    if (error)
        Dialog(message, {"Ok"});
    else
        Dialog("Unknown error occured (probably, simple you aren't root)", {"Ok"});
}
void TConsoleScreen::InfoDialog(std::vector<std::string> lines) {
    unsigned int w = 0;
    unsigned int h = lines.size();
    for (auto &l : lines)
        if (l.length() > w)
            w = l.length();
    int x0 = Width() / 2 - w / 2;
    int y0 = Height() / 2 - h / 2;
    bool done = false;

    WINDOW *win = newwin(h + 2, w + 4, y0 - 1, x0 - 2);
    box(win, 0, 0);
    wrefresh(win);

    while (!done) {
        int n = 0;
        for (auto &l : lines) {
            PrintAt(l, x0, y0 + n, l.length(), false);
            n++;
        }

        switch(Getch()) {
        case 0:
        case -1:
            break;
        default:
            done = true;
            break;
        }

        Refresh();
    }

    delwin(win);
}

void TConsoleScreen::HelpDialog() {
    std::vector<std::string> help =
        {std::string("portoctl top ") + PORTO_VERSION + " " + PORTO_REVISION,
         "",
         "left, right, home, end - change sorting/scroll",
         "up, down, page up, page down - select container/scroll",
         "tab - expand conteainers tree: first, second, all",
         "@ - go to self container",
         "",
         "1-9,0 - set update delay to 1s-9s and 10s",
         "space - pause/resume screen updates",
         "u - update screen",
         "",
         "d, del - disable column",
         "backspace - move column left",
         "f - choose columns",
         "",
         "g - get properties",
         "o - show stdout",
         "e - show stderr",
         "t - run top in container",
         "b - run bash in container",
         "",
         "S - start/stop container",
         "P - pause/resume container",
         "K - kill container",
         "D - destroy container",
         "",
         "q - quit",
         "h,? - help"};
    InfoDialog(help);
}

void TConsoleScreen::ColumnsMenu(std::vector<TColumn> &columns) {
    const int MENU_SPACING = 2;

    const char CHECKED[] = " [*]  ";
    const char UNCHECKED[] = " [ ]  ";
    const int CHECKBOX_SIZE = strlen(CHECKED);

    const int BOX_BORDER = 2;


    int title_width = 0, desc_width = 0;

    for (auto &col : columns) {
        title_width = std::max(title_width, (int)col.Title.length());
        desc_width = std::max(desc_width, (int)col.Description.length());
    }

    int menu_width = title_width + desc_width + MENU_SPACING;
    int win_width = menu_width + BOX_BORDER + CHECKBOX_SIZE + MENU_SPACING;

    const int menu_lines = std::min((int)columns.size(),
                                    std::max(1, Height() - 6));

    const int win_height = menu_lines + BOX_BORDER + 2 + 1;

    int x0 = Width() / 2 - win_width / 2;
    int y0 = Height() / 2 - win_height / 2;

    WINDOW *win = newwin(win_height, win_width, y0, x0);

    box(win, 0, 0);
    wrefresh(win);

    std::vector<ITEM *> items;

    for (auto &col : columns) {
        auto item = new_item(col.Title.c_str(), col.Description.c_str());
        items.push_back(item);
    }

    items.push_back(NULL);

    mvwprintw(win, 1, 2, "Select displayed columns:");

    MENU *menu = new_menu(items.data());
    WINDOW *sub = derwin(win, menu_lines, menu_width, 3, BOX_BORDER / 2 + CHECKBOX_SIZE);

    set_menu_win(menu, win);
    set_menu_sub(menu, sub);
    set_menu_mark(menu, "");
    set_menu_format(menu, menu_lines, 1);
    set_menu_spacing(menu, MENU_SPACING, 0, 0);

    post_menu(menu);

    bool done = false;

    while (!done) {
        for (int i = 0; i < menu_lines; i++) {
            bool hidden = columns[top_row(menu) + i].Hidden;
            mvwprintw(win, 3 + i, 1, hidden ? UNCHECKED : CHECKED);
        }

        wrefresh(win);

        switch(Getch()) {
            case KEY_DOWN:
                menu_driver(menu, REQ_DOWN_ITEM);
                break;
            case KEY_UP:
                menu_driver(menu, REQ_UP_ITEM);
                break;
            case KEY_NPAGE:
                menu_driver(menu, REQ_SCR_DPAGE);
                break;
            case KEY_PPAGE:
                menu_driver(menu, REQ_SCR_UPAGE);
                break;
            case KEY_HOME:
                menu_driver(menu, REQ_FIRST_ITEM);
                break;
            case KEY_END:
                menu_driver(menu, REQ_LAST_ITEM);
                break;
            case 'f':
            case 'q':
            case 'Q':
            case '\n':
                done = true;
                break;
            case ' ':
                {
                    auto &value = columns[item_index(current_item(menu))].Hidden;
                    value = !value;
                }
                break;
        }
    }

    unpost_menu(menu);
    free_menu(menu);

    for (auto &item : items)
        if (item)
            free_item(item);

    delwin(sub);
    delwin(win);
    Refresh();
}

///////////////////////////////////////////////////////

void TPortoValueCache::Register(const std::string &container,
                                const std::string &variable) {
    if (Containers.find(container) == Containers.end())
        Containers[container] = 1;
    else
        Containers[container]++;
    if (Variables.find(variable) == Variables.end())
        Variables[variable] = 1;
    else
        Variables[variable]++;
}
void TPortoValueCache::Unregister(const std::string &container,
                                  const std::string &variable) {
    auto c = Containers.find(container);
    if (c != Containers.end()) {
        if (c->second == 1)
            Containers.erase(c);
        else
            c->second--;
    }
    auto v = Variables.find(variable);
    if (v != Variables.end()) {
        if (v->second == 1)
            Variables.erase(v);
        else
            v->second--;
    }
}

std::string TPortoValueCache::GetValue(const std::string &container,
                                       const std::string &variable,
                                       bool prev) {
    return Cache[CacheSelector ^ prev][container][variable].Value;
}

uint64_t TPortoValueCache::GetDt() {
    return Time[CacheSelector] - Time[!CacheSelector];
}

int TPortoValueCache::Update(Porto::Connection &api) {
    std::vector<std::string> _containers;
    for (auto &iter : Containers)
        _containers.push_back(iter.first);

    std::vector<std::string> _variables;
    for (auto &iter : Variables)
        _variables.push_back(iter.first);

    CacheSelector = !CacheSelector;
    Cache[CacheSelector].clear();
    int ret = api.Get(_containers, _variables, Cache[CacheSelector],
                      Porto::GetFlags::Sync | Porto::GetFlags::Real);
    Time[CacheSelector] = GetCurrentTimeMs();

    api.GetVersion(Version, Revision);

    return ret;
}

TPortoValue::TPortoValue() : Cache(nullptr), Container(nullptr), Flags(ValueFlags::Raw) {
}

TPortoValue::TPortoValue(const TPortoValue &src) :
    Cache(src.Cache), Container(src.Container), Variable(src.Variable), Flags(src.Flags),
    Multiplier(src.Multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::TPortoValue(const TPortoValue &src, std::shared_ptr<TPortoContainer> &container) :
    Cache(src.Cache), Container(container), Variable(src.Variable), Flags(src.Flags),
    Multiplier(src.Multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::TPortoValue(std::shared_ptr<TPortoValueCache> &cache,
                         std::shared_ptr <TPortoContainer> &container,
                         const std::string &variable, int flags, double multiplier) :
    Cache(cache), Container(container), Variable(variable), Flags(flags),
    Multiplier(multiplier) {
    if (Cache && Container)
        Cache->Register(Container->GetName(), Variable);
}

TPortoValue::~TPortoValue() {
    if (Cache && Container)
        Cache->Unregister(Container->GetName(), Variable);

    Container = nullptr;
}

void TPortoValue::Process() {
    if (!Container) {
        AsString = "";
        return;
    }

    if (Flags == ValueFlags::Container) {
        std::string name = Container->GetName();
        std::string tab = "", tag = "";

        int level = Container->GetLevel();

        if (name != "/")
            name = name.substr(1 + name.rfind('/'));

        tab = std::string(level, ' ');

        if (Container->Tag & PortoTreeTags::Self)
            tag = "@ ";

        else if (level)
            tag = Container->ChildrenCount() ? "+ " : "- ";

        AsString = tab + tag + name;
        return;
    }

    AsString = Cache->GetValue(Container->GetName(), Variable, false);

    if (Flags == ValueFlags::State) {
        AsNumber = 0;
        if (AsString == "running")
            AsNumber = 1000;
        else if (AsString == "meta")
            AsNumber = 500;
        else if (AsString == "starting")
            AsNumber = 300;
        else if (AsString == "paused")
            AsNumber = 200;
        else if (AsString == "dead")
            AsNumber = 100;

        AsNumber += Container->ChildrenCount();

        return;
    }

    if (Flags == ValueFlags::Raw || AsString.length() == 0) {
        AsNumber = -1;
        return;
    }

    AsNumber = ParseValue(AsString, Flags & ValueFlags::Map);

    if (Flags & ValueFlags::DfDt) {
        std::string old = Cache->GetValue(Container->GetName(), Variable, true);
        if (old.length() == 0)
            old = AsString;
        AsNumber = DfDt(AsNumber, ParseValue(old, Flags & ValueFlags::Map), Cache->GetDt());
    }

    if (Flags & ValueFlags::PartOfRoot) {
        std::string root_raw = Cache->GetValue("/", Variable, false);
        double root_number;

        root_number = ParseValue(root_raw, Flags & ValueFlags::Map);

        if (Flags & ValueFlags::DfDt) {
            std::string old = Cache->GetValue("/", Variable, true);
            if (old.length() == 0)
                old = root_raw;
            root_number = DfDt(root_number, ParseValue(old, Flags & ValueFlags::Map), Cache->GetDt());
        }

        AsNumber = PartOf(AsNumber, root_number);
    }

    if (Flags & ValueFlags::Multiplier)
        AsNumber /= Multiplier;

    if (Flags & ValueFlags::Percents)
        AsString = StringFormat("%.1f", AsNumber * 100);
    else if (Flags & ValueFlags::Seconds)
        AsString = StringFormatDuration(AsNumber * 1000);
    else if (Flags & ValueFlags::Bytes)
        AsString = StringFormatSize(AsNumber);
    else
        AsString = StringFormat("%g", AsNumber);
}
std::string TPortoValue::GetValue() const {
    return AsString;
}
int TPortoValue::GetLength() const {
    return AsString.length();
}
bool TPortoValue::operator< (const TPortoValue &v) {
    if (Flags == ValueFlags::Raw)
        return AsString < v.AsString;
    else if (Flags == ValueFlags::Container)
        return Container->GetName() < v.Container->GetName();
    else
        return AsNumber > v.AsNumber;
}

TCommonValue::TCommonValue(const std::string &label, const TPortoValue &val) :
    Label(label), Value(val) {
}
std::string TCommonValue::GetLabel() {
    return Label;
}
TPortoValue& TCommonValue::GetValue() {
    return Value;
}

TPortoContainer::TPortoContainer(std::string container) : Container(container) {
    if (Container == "/") {
        Level = 0;
    } else {
        auto unprefixed = container.substr(strlen(ROOT_PORTO_NAMESPACE));
        Level = 1 + std::count(unprefixed.begin(), unprefixed.end(), '/');
    }
}
std::shared_ptr<TPortoContainer> TPortoContainer::GetParent(int level) {
    auto parent = Parent.lock();
    if (parent) {
        if (parent->GetLevel() == level)
            return parent;
        else
            return parent->GetParent(level);
    } else
        return nullptr;
}

std::shared_ptr<TPortoContainer> TPortoContainer::ContainerTree(Porto::Connection &api) {
    std::vector<std::string> containers;
    int ret = api.List(containers);
    if (ret)
        return nullptr;

    std::shared_ptr<TPortoContainer> root = nullptr;
    std::shared_ptr<TPortoContainer> curr = nullptr;
    std::shared_ptr<TPortoContainer> prev = nullptr;
    int level = 0;

    std::string self_absolute_name;
    ret = api.GetProperty("self", "absolute_name", self_absolute_name);
    if (ret)
        return nullptr;

    std::string self_porto_ns;
    ret = api.GetProperty("self", "absolute_namespace", self_porto_ns);
    if (ret)
        return nullptr;

    for (auto &ct : containers)
        ct = self_porto_ns + ct;

    if (self_absolute_name != "/") {
        auto parent = self_absolute_name;
        auto pos = parent.size();

        do {
            auto self_parent = parent.substr(0, pos);

            if (self_parent != "/porto" &&
                std::find(containers.begin(), containers.end(), self_parent)
                          == containers.end()) {

                containers.push_back(self_parent);
            }

            pos = pos ? parent.rfind("/", pos - 1) : std::string::npos;
        } while (pos != std::string::npos && pos);
    }

    std::sort(containers.begin(), containers.end());

    root = std::make_shared<TPortoContainer>("/");
    prev = root;
    root->Tag = self_absolute_name == "/" ? PortoTreeTags::Self : PortoTreeTags::None;

    for (auto &c : containers) {
        if (c == "/")
            continue;

        curr = std::make_shared<TPortoContainer>(c);

        if (c == self_absolute_name)
            curr->Tag |= PortoTreeTags::Self;

        level = curr->GetLevel();
        if (level > prev->GetLevel())
            curr->Parent = prev;
        else if (level == prev->GetLevel())
            curr->Parent = prev->Parent;
        else /* level < prev->GetLevel() */
            curr->Parent = prev->GetParent(level - 1);
        curr->Root = root;

        auto parent = curr->Parent.lock();
        if (!parent)
            return nullptr;

        parent->Children.push_back(curr);
        prev = curr;
    }

    return root;
}
std::string TPortoContainer::GetName() {
    return Container;
}
int TPortoContainer::GetLevel() {
    return Level;
}
void TPortoContainer::ForEach(std::function<void (
                              std::shared_ptr<TPortoContainer> &)> fn, int maxlevel) {

    auto self = shared_from_this();

    if (Level <= maxlevel)
        fn(self);
    if (Level < maxlevel)
        for (auto &c : Children)
            c->ForEach(fn, maxlevel);
}
int TPortoContainer::GetMaxLevel() {
    int level = Level;
    for (auto &c : Children)
        if (c->GetMaxLevel() > level)
            level = c->GetMaxLevel();
    return level;
}
std::string TPortoContainer::ContainerAt(int n, int max_level) {
    auto ret = shared_from_this();
    int i = 0;
    ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            if (i++ == n)
                ret = row;
        }, max_level);
    return ret->GetName();
}
int TPortoContainer::ChildrenCount() {
    return Children.size();
}


TColumn::TColumn(std::string title, std::string desc,
                 TPortoValue var, bool left_aligned, bool hidden) :

    RootValue(var), LeftAligned(left_aligned), Hidden(hidden),
    Title(title), Description(desc) {

    Width = title.length();
}
int TColumn::PrintTitle(int x, int y, TConsoleScreen &screen) {
    screen.PrintAt(Title, x, y, Width, LeftAligned,
                   A_BOLD | (Selected ? A_STANDOUT : 0));
    return Width;
}
int TColumn::Print(TPortoContainer &row, int x, int y, TConsoleScreen &screen, bool selected) {
    std::string p = At(row).GetValue();
    screen.PrintAt(p, x, y, Width, LeftAligned, selected ? A_REVERSE : 0);
    return Width;
}
void TColumn::Update(std::shared_ptr<TPortoContainer> &tree, int maxlevel) {
    tree->ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            TPortoValue val(RootValue, row);
            Cache.insert(std::make_pair(row->GetName(), val));
        }, maxlevel);
}
TPortoValue& TColumn::At(TPortoContainer &row) {
    return Cache[row.GetName()];
}
void TColumn::Highlight(bool enable) {
    Selected = enable;
}
void TColumn::Process() {
    for (auto &iter : Cache) {
        iter.second.Process();

        int w = iter.second.GetLength();
        if (w > Width)
            Width = w;
    }
}
int TColumn::GetWidth() {
    return Width;
}
void TColumn::SetWidth(int width) {
    Width = width;
}
void TColumn::ClearCache() {
    Cache.clear();
}
void TPortoContainer::SortTree(TColumn &column) {
    Children.sort([&] (std::shared_ptr<TPortoContainer> &row1,
                       std::shared_ptr<TPortoContainer> &row2) {
            return column.At(*row1) < column.At(*row2);
        });
    for (auto &c : Children)
        c->SortTree(column);
}

void TPortoTop::PrintTitle(int y, TConsoleScreen &screen) {
    int x = FirstX;
    for (auto &c : Columns)
        if (!c.Hidden)
            x += 1 + c.PrintTitle(x, y, screen);
}
int TPortoTop::PrintCommon(TConsoleScreen &screen) {
    int x = 0;
    int y = 0;
    for (auto &line : Common) {
        for (auto &item : line) {
            std::string p = item.GetLabel();
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = item.GetValue().GetValue();
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
            x += p.length() + 1;
        }

        if (!y) {
            std::string p = "Version: ";
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = Cache->Version;
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
            x += p.length() + 1;

            p = "Update: ";
            screen.PrintAt(p, x, y, p.length());
            x += p.length();
            p = Paused ? "paused" : StringFormatDuration(Delay);
            screen.PrintAt(p, x, y, p.length(), false, A_BOLD);
        }

        y++;
        x = 0;
    }
    return y;
}

void TPortoTop::Update() {
    for (auto &column : Columns)
        column.ClearCache();
    ContainerTree = TPortoContainer::ContainerTree(*Api);
    if (!ContainerTree)
        return;
    for (auto &column : Columns)
        column.Update(ContainerTree, MaxLevel);
    Cache->Update(*Api);
    Process();
}

void TPortoTop::Process() {
    for (auto &column : Columns)
        column.Process();
    for (auto &line : Common)
        for (auto &item : line)
            item.GetValue().Process();
    Sort();
}

void TPortoTop::Sort() {
    if (ContainerTree)
        ContainerTree->SortTree(Columns[SelectedColumn]);
}

void TPortoTop::Print(TConsoleScreen &screen) {

    screen.Erase();

    if (!ContainerTree)
        return;

    int width = 0;
    for (auto &column : Columns)
        if (!column.Hidden)
            width += column.GetWidth();

    if (width > screen.Width()) {
        int excess = width - screen.Width();
        int current = Columns[0].GetWidth();
        if (current > 30) {
            current -= excess;
            if (current < 30)
                current = 30;
        }
        Columns[0].SetWidth(current);
    }

    int at_row = 1 + PrintCommon(screen);

    MaxRows = 0;
    ContainerTree->ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            if (SelectedContainer == "self" && (row->Tag & PortoTreeTags::Self))
                SelectedContainer = row->GetName();
            if (row->GetName() == SelectedContainer)
                SelectedRow = MaxRows;
            MaxRows++;
        }, MaxLevel);
    DisplayRows = std::min(screen.Height() - at_row, MaxRows);
    ChangeSelection(0, 0, screen);

    PrintTitle(at_row - 1, screen);
    int y = 0;
    SelectedContainer = "";
    ContainerTree->ForEach([&] (std::shared_ptr<TPortoContainer> &row) {
            if (y >= FirstRow && y < MaxRows) {
                bool selected = y == SelectedRow;
                if (selected)
                    SelectedContainer = row->GetName();
                int x = FirstX;
                for (auto &c : Columns) {
                    if (!c.Hidden)
                        x += 1 + c.Print(*row, x, at_row + y - FirstRow,
                                         screen, selected);
                }
            }
            y++;
        }, MaxLevel);
    screen.Refresh();
}
void TPortoTop::AddColumn(const TColumn &c) {
    Columns.push_back(c);
}
bool TPortoTop::AddColumn(std::string title, std::string signal,
                          std::string desc, bool hidden) {

    int flags = ValueFlags::Raw;
    size_t off = 0;
    std::string data;

    if (signal == "state")
        flags = ValueFlags::State;

    if (signal.length() > 4 && signal[0] == 'S' && signal[1] == '(') {
        off = signal.find(')');
        data = signal.substr(2, off == std::string::npos ?
                           std::string::npos : off - 2);
        flags |= ValueFlags::Map;
    } else {
        off = signal.find('\'');
        if (off == std::string::npos)
            off = signal.find(' ');
        if (off == std::string::npos)
            off = signal.find('%');

        data = signal.substr(0, off);
    }

    double multiplier = 1;

    if (off != std::string::npos) {
        for (; off < signal.length(); off++) {
            switch (signal[off]) {
            case 'b':
            case 'B':
                flags |= ValueFlags::Bytes;
                break;
            case 's':
            case 'S':
                flags |= ValueFlags::Seconds;
                break;
            case '\'':
                flags |= ValueFlags::DfDt;
                break;
            case '/':
                flags |= ValueFlags::PartOfRoot;
                break;
            case '%':
                flags |= ValueFlags::Percents;
                break;
            case ' ':
                break;
            default:
                try {
                    size_t tmp;
                    multiplier = stod(signal.substr(off), &tmp);
                    off += tmp - 1;
                    flags |= ValueFlags::Multiplier;
                } catch (...) {
                }
                break;
            }
        }
    }

    TPortoValue v(Cache, RootContainer, data, flags, multiplier);
    Columns.push_back(TColumn(title, desc, v, false, hidden));
    return true;
}

void TPortoTop::ChangeSelection(int x, int y, TConsoleScreen &screen) {
    SelectedRow += y;

    if (SelectedRow < 0)
        SelectedRow = 0;

    if (SelectedRow >= MaxRows)
        SelectedRow = MaxRows - 1;

    if (SelectedRow < FirstRow)
        FirstRow = SelectedRow;

    if (SelectedRow >= FirstRow + DisplayRows)
        FirstRow = SelectedRow - DisplayRows + 1;

    if (FirstRow + DisplayRows > MaxRows)
        FirstRow = MaxRows - DisplayRows;

    Columns[SelectedColumn].Highlight(false);
    SelectedColumn += x;
    if (SelectedColumn < 0) {
        SelectedColumn = 0;
    } else if (SelectedColumn > (int)Columns.size() - 1) {
        SelectedColumn = Columns.size() - 1;
    }
    while (Columns[SelectedColumn].Hidden && x < 0 && SelectedColumn > 0)
        SelectedColumn--;
    while (Columns[SelectedColumn].Hidden && SelectedColumn < Columns.size() - 1)
        SelectedColumn++;
    while (Columns[SelectedColumn].Hidden && SelectedColumn > 0)
        SelectedColumn--;
    Columns[SelectedColumn].Highlight(true);

    if (x)
        Sort();

    if (y)
        SelectedContainer = "";

    if (x == 0 && y == 0) {
        int i = 0;
        int _x = FirstX;
        for (auto &c : Columns) {
            if (i == SelectedColumn && _x <= 0) {
                FirstX -= _x;
                _x = 0;
            }
            if (!c.Hidden)
                _x += c.GetWidth() + 1;
            if (i == SelectedColumn && _x > screen.Width()) {
                FirstX -= _x - screen.Width();
                _x = screen.Width();
            }
            i++;
        }
        if (FirstX < 0 && _x < screen.Width())
            FirstX += std::min(screen.Width() - _x, -FirstX);
    }
}
void TPortoTop::Expand() {
    if (MaxLevel == 1)
        MaxLevel = 2;
    else if (MaxLevel == 2)
        MaxLevel = 100;
    else
        MaxLevel = 1;
    Update();
}
int TPortoTop::StartStop() {
    std::string state;
    int ret = Api->GetProperty(SelectedContainer, "state", state);
    if (ret)
        return ret;
    if (state == "running" || state == "dead" || state == "meta")
        return Api->Stop(SelectedContainer);
    else
        return Api->Start(SelectedContainer);
}
int TPortoTop::PauseResume() {
    std::string state;
    int ret = Api->GetProperty(SelectedContainer, "state", state);
    if (ret)
        return ret;
    if (state == "paused")
        return Api->Resume(SelectedContainer);
    else
        return Api->Pause(SelectedContainer);
}
int TPortoTop::Kill(int signal) {
    return Api->Kill(SelectedContainer, signal);
}
int TPortoTop::Destroy() {
    return Api->Destroy(SelectedContainer);
}
void TPortoTop::LessPortoctl(std::string container, std::string cmd) {
    std::string s(program_invocation_name);
    s += " get " + container + " " + cmd + " | less";
    int status = system(s.c_str());
    (void)status;
}

int TPortoTop::RunCmdInContainer(TConsoleScreen &screen, std::string cmd) {
    bool enter = (SelectedContainer != "/" && SelectedContainer != "self");
    int ret = -1;

    screen.Save();
    switch (fork()) {
    case -1:
        ret = errno;
        break;
    case 0:
    {
        if (enter)
            exit(execlp(program_invocation_name, program_invocation_name,
                        "shell", SelectedContainer.c_str(), cmd.c_str(), nullptr));
        else
            exit(execlp(cmd.c_str(), cmd.c_str(), nullptr));
        break;
    }
    default:
    {
        wait(&ret);
        break;
    }
    }
    screen.Restore();

    if (ret)
        screen.Dialog(strerror(ret), {"Ok"});

    return ret;
}
void TPortoTop::AddCommon(int row, const std::string &title, const std::string &var,
                          std::shared_ptr<TPortoContainer> &container,
                          int flags, double multiplier) {
    Common.resize(row + 1);
    TPortoValue v(Cache, container, var, flags, multiplier);
    Common[row].push_back(TCommonValue(title, v));
}
TPortoTop::TPortoTop(Porto::Connection *api, const std::vector<std::string> &args) :
    Api(api),
    Cache(std::make_shared<TPortoValueCache>()),
    RootContainer(std::make_shared<TPortoContainer>("/")) {

    (void)args;

    AddCommon(0, "Containers running: ", "porto_stat[running]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "of ", "porto_stat[containers]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Volumes: ", "porto_stat[volumes]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Clients: ", "porto_stat[clients]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Errors: ", "porto_stat[errors]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "Warnings: ", "porto_stat[warnings]", RootContainer, ValueFlags::Raw);
    AddCommon(0, "RPS: ", "porto_stat[requests_completed]", RootContainer, ValueFlags::DfDt);
    AddCommon(0, "Uptime: ", "porto_stat[porto_uptime]", RootContainer, ValueFlags::Seconds);

    AddColumn(TColumn("container", "Container name",
              TPortoValue(Cache, ContainerTree, "absolute_name", ValueFlags::Container), true, false));

    AddColumn("state", "state", "Current state");
    AddColumn("time", "time s", "Time elapsed since start or death");

    /* CPU */
    AddColumn("policy", "cpu_policy", "Cpu policy being used");
    AddColumn("cpu%", "cpu_usage'% 1e9", "Cpu usage in core%");
    AddColumn("limit", "cpu_limit", "Cpu limit in cores");
    AddColumn("sys%", "cpu_usage_system'% 1e9", "System cpu usage in core%");
    AddColumn("wait%", "cpu_wait'% 1e9", "Cpu wait time in core%");
    AddColumn("g-e", "cpu_guarantee", "Cpu guarantee in cores");

    /* Memory */
    AddColumn("memory", "memory_usage b", "Memory usage");
    AddColumn("limit", "memory_limit b", "Memory limit");
    AddColumn("g-e", "memory_guarantee b", "Memory guarantee");
    AddColumn("anon", "anon_usage b", "Anonymous memory usage");
    AddColumn("alim", "anon_limit b", "Anonymous memory limit");
    AddColumn("cache", "cache_usage b", "Cache memory usage");

    AddColumn("threads", "thread_count", "Threads count");
    AddColumn("oom", "porto_stat[container_oom]", "OOM count");

    /* I/O */
    AddColumn("maj/s", "major_faults'", "Major page fault count");
    AddColumn("read b/s", "io_read[hw]' b", "IO bytes read from disk");
    AddColumn("write b/s", "io_write[hw]' b", "IO bytes written to disk");
    AddColumn("io op/s", "io_ops[hw]'", "IO operations per second");
    AddColumn("io load", "io_time[hw]' 1e9", "Average disk queue depth");
    AddColumn("fs read b/s", "io_read[fs]' b", "IO bytes read by fs");
    AddColumn("fs write b/s", "io_write[fs]' b", "IO bytes written by fs");
    AddColumn("fs iop/s", "io_ops[fs]'", "IO operations by fs");

    /* Network */
    AddColumn("net", "S(net_bytes) 'b", "Bytes transmitted by container");
    AddColumn("pkt", "S(net_packets)'", "Packets transmitted by container");
    AddColumn("drop", "S(net_drops)'", "Packets dropped by container");

    AddColumn("net rx", "S(net_rx_bytes) 'b", "Bytes received by interfaces");
    AddColumn("pkt rx", "S(net_rx_packets)'", "Packets received by interfaces");
    AddColumn("drop rx", "S(net_rx_drops)'", "Outcomming packets dropped by interfaces");

    AddColumn("net tx", "S(net_tx_bytes) 'b", "Bytes transmitted by interfaces");
    AddColumn("pkt tx", "S(net_tx_packets)'", "Packets transmitted by interfaces");
    AddColumn("drop tx", "S(net_tx_drops)'", "Incomming packets dropped by interfaces");
}

static bool exit_immediatly = false;
void exit_handler(int) {
    exit_immediatly = true;
}

int portotop(Porto::Connection *api, const std::vector<std::string> &args) {
    Signal(SIGINT, exit_handler);
    Signal(SIGTERM, exit_handler);
    Signal(SIGTTOU, SIG_IGN);
    Signal(SIGTTIN, SIG_IGN);

    TPortoTop top(api, args);

    top.SelectedContainer = "self";

    top.Update();

    /* Main loop */
    TConsoleScreen screen;

    bool first = true;

    screen.SetTimeout(top.FirstDelay);

    while (true) {
        if (exit_immediatly)
            break;

        top.Print(screen);

        int button = screen.Getch();
        switch (button) {
        case ERR:
            if (!top.Paused)
                top.Update();
            break;
        case 'q':
        case 'Q':
            return EXIT_SUCCESS;
            break;
        case KEY_UP:
            top.ChangeSelection(0, -1, screen);
            break;
        case KEY_PPAGE:
            top.ChangeSelection(0, -10, screen);
            break;
        case KEY_DOWN:
            top.ChangeSelection(0, 1, screen);
            break;
        case KEY_NPAGE:
            top.ChangeSelection(0, 10, screen);
            break;
        case KEY_LEFT:
            top.ChangeSelection(-1, 0, screen);
            break;
        case KEY_RIGHT:
            top.ChangeSelection(1, 0, screen);
            break;
        case KEY_HOME:
            top.ChangeSelection(-1000, 0, screen);
            break;
        case KEY_END:
            top.ChangeSelection(1000, 0, screen);
            break;
        case '\t':
            top.Expand();
            break;
        case ' ':
            top.Paused = !top.Paused;
            break;
        case 'f':
            screen.ColumnsMenu(top.Columns);
            break;
        case KEY_DC:
        case 'd':
            if (top.SelectedColumn > 0)
                top.Columns[top.SelectedColumn].Hidden ^= true;
            break;
        case KEY_BACKSPACE:
            if (top.SelectedColumn > 1) {
                top.SelectedColumn--;
                std::swap(top.Columns[top.SelectedColumn],
                          top.Columns[top.SelectedColumn + 1]);
            }
            break;
        case 'S':
            if (screen.Dialog("Start/stop container " + top.SelectedContainer,
                              {"No", "Yes"}) == 1) {
                if (top.StartStop())
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        case 'P':
            if (screen.Dialog("Pause/resume container " + top.SelectedContainer,
                              {"No", "Yes"}) == 1) {
                if (top.PauseResume())
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        case 'K':
        {
            int signal = -1;
            switch (screen.Dialog("Kill container " + top.SelectedContainer,
                                  {"Cancel", "SIGTERM", "SIGINT", "SIGKILL", "SIGHUP"})) {
            case 1:
                signal = SIGTERM;
                break;
            case 2:
                signal = SIGINT;
                break;
            case 3:
                signal = SIGKILL;
                break;
            case 4:
                signal = SIGHUP;
                break;
            }
            if (signal > 0) {
                if (top.Kill(signal))
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        }
        case 'D':
            if (screen.Dialog("Destroy container " + top.SelectedContainer,
                              {"No", "Yes"}) == 1) {
                if (top.Destroy())
                    screen.ErrorDialog(*api);
                else
                    top.Update();
            }
            break;
        case 't':
            top.RunCmdInContainer(screen, "top");
            break;
        case 'b':
            top.RunCmdInContainer(screen, "bash");
            break;
        case 'g':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer, "");
            screen.Restore();
            break;
        case 'o':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer, "stdout");
            screen.Restore();
            break;
        case 'e':
            screen.Save();
            top.LessPortoctl(top.SelectedContainer, "stderr");
            screen.Restore();
            break;
        case '0':
            top.Delay = 10000;
            top.Paused = false;
            screen.SetTimeout(top.Delay);
            break;
        case '1'...'9':
            top.Delay = (button - '0') * 1000;
            top.Paused = false;
            screen.SetTimeout(top.Delay);
            break;
        case 'u':
            top.Update();
            screen.Clear();
            break;
        case '@':
            top.SelectedContainer = "self";
            break;
        case 0:
        case KEY_RESIZE:
        case KEY_MOUSE:
            break;
        case 'h':
        case '?':
        default:
            screen.HelpDialog();
            break;
        }

        if (first) {
            first = false;
            screen.SetTimeout(top.Delay);
        }
    }

    return EXIT_SUCCESS;
}
