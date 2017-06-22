#include <algorithm>
#include <unordered_map>
#include <sstream>
#include <fstream>

#include "network.hpp"
#include "container.hpp"
#include "config.hpp"
#include "client.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/crc32.hpp"

extern "C" {
#include <linux/if.h>
#include <netinet/ip6.h>
#include <netinet/ether.h>
#include <netlink/route/addr.h>
#include <netlink/route/class.h>
#include <netlink/route/qdisc/htb.h>
}

TNetWorker NetWorker;
std::shared_ptr<TNetwork> HostNetwork;
static std::unordered_map<ino_t, std::weak_ptr<TNetwork>> Networks;
static std::mutex NetworksMutex;

static std::vector<std::string> UnmanagedDevices;
static std::vector<int> UnmanagedGroups;
static std::map<int, std::string> DeviceGroups;

static TStringMap DeviceQdisc;
static TUintMap DeviceRate;
static TUintMap DeviceCeil;
static TUintMap DeviceRateBurst;
static TUintMap DeviceCeilBurst;
static TUintMap DeviceQuantum;

static TUintMap PortoRate;

static TUintMap DefaultRate;
static TStringMap DefaultQdisc;
static TUintMap DefaultQdiscLimit;
static TUintMap DefaultQdiscQuantum;

static TUintMap ContainerRate;
static TStringMap ContainerQdisc;
static TUintMap ContainerQdiscLimit;
static TUintMap ContainerQdiscQuantum;

static TUintMap IngressBurst;

static uint64_t NetworkRefreshPeriod;

static inline std::unique_lock<std::mutex> LockNetworks() {
    return std::unique_lock<std::mutex>(NetworksMutex);
}

static std::list<std::pair<std::string, std::string>> NetSysctls = {
    { "net.core.somaxconn", "128" },

    { "net.unix.max_dgram_qlen", "10" },

    { "net.ipv4.icmp_echo_ignore_all", "0" },
    { "net.ipv4.icmp_echo_ignore_broadcasts", "1" },
    { "net.ipv4.icmp_ignore_bogus_error_responses", "1" },
    { "net.ipv4.icmp_errors_use_inbound_ifaddr", "0" },
    { "net.ipv4.icmp_ratelimit", "1000" },
    { "net.ipv4.icmp_ratemask", "6168" },
    { "net.ipv4.ping_group_range", "1\t0" },

    { "net.ipv4.tcp_ecn", "2" },
    { "net.ipv4.tcp_ecn_fallback", "1" },
    { "net.ipv4.ip_dynaddr", "0" },
    { "net.ipv4.ip_early_demux", "1" },
    { "net.ipv4.ip_default_ttl", "64" },

    { "net.ipv4.ip_local_port_range", "32768\t60999" },
    { "net.ipv4.ip_local_reserved_ports", "" },
    { "net.ipv4.ip_no_pmtu_disc", "0" },
    { "net.ipv4.ip_forward_use_pmtu", "0" },
    { "net.ipv4.ip_nonlocal_bind", "0" },
    //"net.ipv4.fwmark_reflect",
    //"net.ipv4.tcp_fwmark_accept",
    { "net.ipv4.tcp_mtu_probing", "0" },
    { "net.ipv4.tcp_base_mss", "1024" },
    { "net.ipv4.tcp_probe_threshold", "8" },
    { "net.ipv4.tcp_probe_interval", "600" },

    //"net.ipv4.igmp_link_local_mcast_reports",
    //"net.ipv4.igmp_max_memberships",
    //"net.ipv4.igmp_max_msf",
    //"net.ipv4.igmp_qrv",

    { "net.ipv4.tcp_keepalive_time", "7200" },
    { "net.ipv4.tcp_keepalive_probes", "9" },
    { "net.ipv4.tcp_keepalive_intvl", "75" },
    { "net.ipv4.tcp_syn_retries", "6" },
    { "net.ipv4.tcp_synack_retries", "5" },
    { "net.ipv4.tcp_syncookies", "1" },
    { "net.ipv4.tcp_reordering", "3" },
    { "net.ipv4.tcp_retries1", "3" },
    { "net.ipv4.tcp_retries2", "15" },
    { "net.ipv4.tcp_orphan_retries", "0" },
    { "net.ipv4.tcp_fin_timeout", "60" },
    { "net.ipv4.tcp_notsent_lowat", "-1" },
    { "net.ipv4.tcp_tw_reuse", "0" },

    { "net.ipv6.bindv6only", "0" },
    //"net.ipv6.anycast_src_echo_reply",
    //"net.ipv6.flowlabel_consistency",
    //"net.ipv6.auto_flowlabels",
    //"net.ipv6.fwmark_reflect",
    //"net.ipv6.idgen_retries",
    //"net.ipv6.idgen_delay",
    //"net.ipv6.flowlabel_state_ranges",
    { "net.ipv6.ip_nonlocal_bind", "0" },

    { "net.ipv6.icmp.ratelimit", "1000" },

    { "net.ipv6.route.gc_thresh", "1024" },
    { "net.ipv6.route.max_size", "4096" },
    { "net.ipv6.route.gc_min_interval", "0" },
    { "net.ipv6.route.gc_timeout", "60" },
    { "net.ipv6.route.gc_interval", "30" },
    { "net.ipv6.route.gc_elasticity", "9" },
    { "net.ipv6.route.mtu_expires", "600" },
    { "net.ipv6.route.min_adv_mss", "1220" },
    { "net.ipv6.route.gc_min_interval_ms", "500" },
};

bool TNetwork::NamespaceSysctl(const std::string &key) {
    for (auto &pair: NetSysctls) {
        if (pair.first == key)
            return true;
    }
    if (StringStartsWith(key, "net.ipv4.conf."))
        return true;
    if (StringStartsWith(key, "net.ipv6.conf."))
        return true;
    if (StringStartsWith(key, "net.ipv4.neigh.") &&
            !StringStartsWith(key, "net.ipv4.neigh.default."))
        return true;
    if (StringStartsWith(key, "net.ipv6.neigh.") &&
            !StringStartsWith(key, "net.ipv6.neigh.default."))
        return true;
    return false;
}

TNetworkDevice::TNetworkDevice(struct rtnl_link *link) {
    Name = rtnl_link_get_name(link);
    Type = rtnl_link_get_type(link) ?: "";
    Index = rtnl_link_get_ifindex(link);
    Link = rtnl_link_get_link(link);
    Group = rtnl_link_get_group(link);
    MTU = rtnl_link_get_mtu(link);

    if (DeviceGroups.count(Group))
        GroupName = DeviceGroups[Group];
    else
        GroupName = std::to_string(Group);

    Rate = NET_MAX_RATE;
    Ceil = NET_MAX_RATE;

    Managed = true;
    Prepared = false;
    Missing = false;

    for (auto &pattern: UnmanagedDevices)
        if (StringMatch(Name, pattern))
            Managed = false;

    if (std::find(UnmanagedGroups.begin(), UnmanagedGroups.end(), Group) !=
            UnmanagedGroups.end())
        Managed = false;
}

std::string TNetworkDevice::GetDesc(void) const {
    return std::to_string(Index) + ":" + Name + " (" + Type + ")";
}

uint64_t TNetworkDevice::GetConfig(const TUintMap &cfg, uint64_t def) const {
    for (auto &it: cfg) {
        if (StringMatch(Name, it.first))
            return it.second;
    }
    auto it = cfg.find("group " + GroupName);
    if (it == cfg.end())
        it = cfg.find("default");
    if (it != cfg.end())
        return it->second;
    return def;
}

std::string TNetworkDevice::GetConfig(const TStringMap &cfg, std::string def) const {
    for (auto &it: cfg) {
        if (StringMatch(Name, it.first))
            return it.second;
    }
    auto it = cfg.find("group " + GroupName);
    if (it == cfg.end())
        it = cfg.find("default");
    if (it != cfg.end())
        return it->second;
    return def;
}

void TNetwork::AddNetwork(ino_t inode, std::shared_ptr<TNetwork> &net) {
    auto lock = LockNetworks();
    Networks[inode] = net;

    for (auto it = Networks.begin(); it != Networks.end(); ) {
        if (it->second.expired())
            it = Networks.erase(it);
        else
            it++;
    }
}

std::shared_ptr<TNetwork> TNetwork::GetNetwork(ino_t inode) {
    auto lock = LockNetworks();
    auto it = Networks.find(inode);
    if (it != Networks.end())
        return it->second.lock();
    return nullptr;
}

void TNetwork::InitializeConfig() {
    std::ifstream groupCfg("/etc/iproute2/group");
    int id;
    std::string name;
    std::map<std::string, int> groupMap;

    while (groupCfg >> std::ws) {
        if (groupCfg.peek() != '#' && (groupCfg >> id >> name)) {
            L_SYS("Network device group: {} : {}", id, name);
            groupMap[name] = id;
            DeviceGroups[id] = name;
        }
        groupCfg.ignore(1 << 16, '\n');
    }

    UnmanagedDevices.clear();
    UnmanagedGroups.clear();

    for (auto device: config().network().unmanaged_device()) {
        L_SYS("Unmanaged network device: {}", device);
        UnmanagedDevices.push_back(device);
    }

    for (auto group: config().network().unmanaged_group()) {
        int id;

        if (groupMap.count(group)) {
            id = groupMap[group];
        } else if (StringToInt(group, id)) {
            L_SYS("Unknown network device group: {}", group);
            continue;
        }

        L_SYS("Unmanaged network device group: {} : {}", id, group);
        UnmanagedGroups.push_back(id);
    }

    if (config().network().has_device_qdisc())
        StringToStringMap(config().network().device_qdisc(), DeviceQdisc);
    if (config().network().has_device_rate())
        StringToUintMap(config().network().device_rate(), DeviceRate);
    if (config().network().has_device_ceil())
        StringToUintMap(config().network().device_rate(), DeviceCeil);
    if (config().network().has_default_rate())
        StringToUintMap(config().network().default_rate(), DefaultRate);
    if (config().network().has_porto_rate())
        StringToUintMap(config().network().porto_rate(), PortoRate);
    if (config().network().has_container_rate())
        StringToUintMap(config().network().container_rate(), ContainerRate);
    if (config().network().has_device_quantum())
        StringToUintMap(config().network().device_quantum(), DeviceQuantum);
    if (config().network().has_device_rate_burst())
        StringToUintMap(config().network().device_rate_burst(), DeviceRateBurst);
    if (config().network().has_device_ceil_burst())
        StringToUintMap(config().network().device_ceil_burst(), DeviceCeilBurst);

    if (config().network().has_default_qdisc())
        StringToStringMap(config().network().default_qdisc(), DefaultQdisc);
    if (config().network().has_default_qdisc_limit())
        StringToUintMap(config().network().default_qdisc_limit(), DefaultQdiscLimit);
    if (config().network().has_default_qdisc_quantum())
        StringToUintMap(config().network().default_qdisc_quantum(), DefaultQdiscQuantum);

    if (config().network().has_container_qdisc())
        StringToStringMap(config().network().container_qdisc(), ContainerQdisc);
    if (config().network().has_container_qdisc_limit())
        StringToUintMap(config().network().container_qdisc_limit(), ContainerQdiscLimit);
    if (config().network().has_container_qdisc_quantum())
        StringToUintMap(config().network().container_qdisc_quantum(), ContainerQdiscQuantum);

    if (config().network().has_ingress_burst())
        StringToUintMap(config().network().ingress_burst(), IngressBurst);

    NetworkRefreshPeriod = config().network().watchdog_ms();

    /* Load default net sysctl from host config */
    for (const auto &p: NetSysctls) {
        auto &key = p.first;
        auto &def = p.second;
        bool set = false;
        for (const auto &it: config().container().net_sysctl())
            set |= it.key() == key;
        std::string val;
        if (!set && !GetSysctl(key, val) && val != def) {
            L("Init sysctl {} = {} (default is {})", key, val, def);
            auto sysctl = config().mutable_container()->add_net_sysctl();
            sysctl->set_key(key);
            sysctl->set_val(val);
        }
    }
}

TError TNetwork::Destroy() {
    auto lock = ScopedLock();
    TError error;

    L_ACT("Removing network...");

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;

        TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
        error = qdisc.Delete(*Nl);
        if (error)
            L_ERR("Cannot remove root qdisc: {}", error);
    }

    return TError::Success();
}

void TNetwork::GetDeviceSpeed(TNetworkDevice &dev) const {
    TPath knob("/sys/class/net/" + dev.Name + "/speed");
    uint64_t speed, rate, ceil;
    std::string text;

    if (ManagedNamespace) {
        dev.Ceil = NET_MAX_RATE;
        dev.Rate = NET_MAX_RATE;
        return;
    }

    if (knob.ReadAll(text) || StringToUint64(text, speed) || speed < 100) {
        ceil = NET_MAX_RATE;
        rate = NET_MAX_RATE;
    } else {
        ceil = speed * 125000; /* Mbit -> Bps */
        rate = speed * 112500; /* 90% */
    }

    dev.Ceil = dev.GetConfig(DeviceCeil, ceil);
    dev.Rate = dev.GetConfig(DeviceRate, rate);
}


TError TNetwork::CreateIngressQdisc(TUintMap &rate) {
    TError error;

    L("Setting up ingress qdisc");

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;

        TNlQdisc ingress(dev.Index, TC_H_INGRESS, TC_H_MAJ(TC_H_INGRESS));
        (void)ingress.Delete(*Nl);

        if (!dev.GetConfig(rate))
            continue;

        ingress.Kind = "ingress";

        error = ingress.Create(*Nl);
        if (error) {
            L_WRN("Cannot create ingress qdisc: {}", error);
            return error;
        }

        TNlPoliceFilter police(dev.Index, TC_H_INGRESS, TC_H_MAJ(TC_H_INGRESS));
        (void)police.Delete(*Nl);

        police.Mtu = 65536; /* maximum GRO skb */
        police.Rate = dev.GetConfig(rate);
        police.Burst = dev.GetConfig(IngressBurst,
                std::max(police.Mtu * 10, police.Rate / 10));

        error = police.Create(*Nl);
        if (error)
            L_WRN("Can't create ingress filter: {}", error);
    }

    return error;
}

TError TNetwork::SetupQueue(TNetworkDevice &dev) {
    TError error;

    //
    // 1:0 qdisc (hfsc)
    //  |
    // 1:1 / class
    //  |
    //  +- 1:2 default class
    //  |   |
    //  |   +- 2:0 default class qdisc (sfq)
    //  |
    //  +- 1:4 container a
    //  |   |
    //  |   +- 1:4004 leaf a
    //  |   |   |
    //  |   |   +- 4:0 qdisc a (pfifo)
    //  |   |
    //  |   +- 1:5 container a/b
    //  |       |
    //  |       +- 1:4005 leaf a/b
    //  |           |
    //  |           +- 5:0 qdisc a/b (pfifo)
    //  |
    //  +- 1:6 container b
    //      |
    //      +- 1:4006 leaf b
    //          |
    //          +- 6:0 qdisc b (pfifo)


    L("Setup queue for network device {}", dev.GetDesc());

    TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
    qdisc.Kind = dev.GetConfig(DeviceQdisc);
    qdisc.Default = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    qdisc.Quantum = 10;

    if (!qdisc.Check(*Nl)) {
        (void)qdisc.Delete(*Nl);
        error = qdisc.Create(*Nl);
        if (error) {
            L_ERR("Cannot create root qdisc: {}", error);
            return error;
        }
    }

    TNlCgFilter filter(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR), 1);
    (void)filter.Delete(*Nl);

    error = filter.Create(*Nl);
    if (error) {
        L_ERR("Can't create tc filter: {}", error);
        return error;
    }

    GetDeviceSpeed(dev);

    TNlClass cls;

    cls.Kind = dev.GetConfig(DeviceQdisc);
    cls.Index = dev.Index;
    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Prio = NET_DEFAULT_PRIO;
    cls.Rate = dev.Ceil;
    cls.Ceil = dev.Ceil;

    error = cls.Create(*Nl);
    if (error) {
        L_ERR("Can't create root tclass: {}", error);
        return error;
    }

    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    cls.Rate = dev.GetConfig(DefaultRate);
    cls.Ceil = 0;

    error = cls.Create(*Nl);
    if (error) {
        L_ERR("Can't create default tclass: {}", error);
        return error;
    }

    if (ManagedNamespace) {
        TNlQdisc defq(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR),
                      TC_HANDLE(DEFAULT_TC_MAJOR, ROOT_TC_MINOR));
        defq.Kind = dev.GetConfig(ContainerQdisc);
        defq.Limit = dev.GetConfig(ContainerQdiscLimit, dev.MTU * 20);
        defq.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 2);
        if (!defq.Check(*Nl)) {
            error = defq.Create(*Nl);
            if (error)
                return error;
        }
    }

    if (this == HostNetwork.get()) {
        RootContainer->NetLimit[dev.Name] = dev.Ceil;
        RootContainer->NetGuarantee[dev.Name] = dev.Rate;
    }

    return TError::Success();
}

TNetwork::TNetwork() : NatBitmap(0, 0) {
    Nl = std::make_shared<TNl>();
    PORTO_ASSERT(Nl != nullptr);
}

TNetwork::~TNetwork() {
}

TError TNetwork::Connect() {
    return Nl->Connect();
}

TError TNetwork::ConnectNetns(TNamespaceFd &netns) {
    TNamespaceFd my_netns;
    TError error;

    error = my_netns.Open(GetTid(), "ns/net");
    if (error)
        return error;

    error = netns.SetNs(CLONE_NEWNET);
    if (error)
        return error;

    error = Connect();

    TError error2 = my_netns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

TError TNetwork::ConnectNew(TNamespaceFd &netns) {
    TNamespaceFd my_netns;
    TError error;

    error = my_netns.Open(GetTid(), "ns/net");
    if (error)
        return error;

    if (unshare(CLONE_NEWNET))
        return TError(EError::Unknown, errno, "unshare(CLONE_NEWNET)");

    error = netns.Open(GetTid(), "ns/net");
    if (!error) {
        error = Connect();
        if (error)
            netns.Close();
    }

    TError error2 = my_netns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    for (auto &al: config().network().addrlabel()) {
        TNlAddr prefix;
        error = prefix.Parse(AF_UNSPEC, al.prefix());
        if (error)
            break;
        error = Nl->AddrLabel(prefix, al.label());
        if (error)
            break;
    }

    return error;
}

TError TNetwork::RefreshDevices(bool force) {
    struct nl_cache *cache;
    TError error;
    int ret;

    ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate link cache");

    for (auto &dev: Devices)
        dev.Missing = true;

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
        auto link = (struct rtnl_link *)obj;
        int flags = rtnl_link_get_flags(link);

        if (flags & IFF_LOOPBACK)
            continue;

        /* Do not setup queue on down links in host namespace */
        if (!ManagedNamespace && !(flags & IFF_RUNNING))
            continue;

        TNetworkDevice dev(link);

        /* Ignore our veth pairs */
        if (dev.Type == "veth" &&
            (StringStartsWith(dev.Name, "portove-") ||
             StringStartsWith(dev.Name, "L3-")))
            continue;

        /* In managed namespace we control all devices */
        if (ManagedNamespace)
            dev.Managed = true;

        dev.Stats.RxBytes = rtnl_link_get_stat(link, RTNL_LINK_RX_BYTES);
        dev.Stats.RxPackets = rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS);
        dev.Stats.RxDrops = rtnl_link_get_stat(link, RTNL_LINK_RX_DROPPED);

        dev.Stats.TxBytes = rtnl_link_get_stat(link, RTNL_LINK_TX_BYTES);
        dev.Stats.TxPackets = rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS);
        dev.Stats.TxDrops = rtnl_link_get_stat(link, RTNL_LINK_TX_DROPPED);

        bool found = false;
        for (auto &d: Devices) {
            if (d.Name != dev.Name || d.Index != dev.Index)
                continue;
            d = dev;
            if (d.Managed && std::string(rtnl_link_get_qdisc(link) ?: "") !=
                    dev.GetConfig(DeviceQdisc))
                Nl->Dump("Detected missing qdisc", link);
            else if (!force)
                d.Prepared = true;

            found = true;
            d.Stats = dev.Stats;
            break;
        }
        if (!found) {
            Nl->Dump("New network device", link);
            if (!dev.Managed)
                L("Unmanaged device {}", dev.GetDesc());
            Devices.push_back(dev);
        }
    }

    nl_cache_free(cache);

    for (auto dev = Devices.begin(); dev != Devices.end(); ) {
        if (dev->Missing) {
            L("Delete network device {}", dev->GetDesc());
            if (this == HostNetwork.get()) {
                RootContainer->NetLimit.erase(dev->Name);
                RootContainer->NetGuarantee.erase(dev->Name);
            }
            dev = Devices.erase(dev);
        } else
            dev++;
    }

    for (auto &dev: Devices) {
        if (!dev.Managed || dev.Prepared)
            continue;
        error = SetupQueue(dev);
        if (error)
            return error;
        dev.Prepared = true;
        NewManagedDevices = true;
    }


    return TError::Success();
}

TError TNetwork::GetGateAddress(std::vector<TNlAddr> addrs,
                                TNlAddr &gate4, TNlAddr &gate6,
                                int &mtu, int &group) {
    struct nl_cache *cache, *lcache;
    int ret;

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &lcache);
    if (ret < 0) {
        nl_cache_free(cache);
        return Nl->Error(ret, "Cannot allocate link cache");
    }

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
         auto addr = (struct rtnl_addr *)obj;
         auto local = rtnl_addr_get_local(addr);

         if (!local || rtnl_addr_get_scope(addr) == RT_SCOPE_HOST)
             continue;

         for (auto &a: addrs) {

             if (nl_addr_get_family(a.Addr) == nl_addr_get_family(local)) {

                 /* get any gate of required family */
                 if (nl_addr_get_family(local) == AF_INET && !gate4.Addr)
                     gate4 = TNlAddr(local);

                 if (nl_addr_get_family(local) == AF_INET6 && !gate6.Addr)
                     gate6 = TNlAddr(local);
             }

             if (nl_addr_cmp_prefix(local, a.Addr) == 0) {

                 /* choose best matching gate address */
                 if (nl_addr_get_family(local) == AF_INET &&
                         nl_addr_cmp_prefix(gate4.Addr, a.Addr) != 0)
                     gate4 = TNlAddr(local);

                 if (nl_addr_get_family(local) == AF_INET6 &&
                         nl_addr_cmp_prefix(gate6.Addr, a.Addr) != 0)
                     gate6 = TNlAddr(local);

                 auto link = rtnl_link_get(lcache, rtnl_addr_get_ifindex(addr));
                 if (link) {
                     int link_mtu = rtnl_link_get_mtu(link);

                     if (mtu < 0 || link_mtu < mtu)
                         mtu = link_mtu;

                     if (!group)
                         group = rtnl_link_get_group(link);

                     rtnl_link_put(link);
                 }
             }
         }
    }

    nl_cache_free(lcache);
    nl_cache_free(cache);

    if (gate4.Addr)
        nl_addr_set_prefixlen(gate4.Addr, 32);

    if (gate6.Addr)
        nl_addr_set_prefixlen(gate6.Addr, 128);

    return TError::Success();
}

TError TNetwork::AddAnnounce(const TNlAddr &addr, std::string master) {
    struct nl_cache *cache;
    TError error;
    int ret;

    if (master != "") {
        int index = DeviceIndex(master);
        if (index)
            return Nl->ProxyNeighbour(index, addr, true);
        return TError(EError::InvalidValue, "Master link not found: " + master);
    }

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    for (auto &dev : Devices) {
        bool reachable = false;

        for (auto obj = nl_cache_get_first(cache); obj;
                obj = nl_cache_get_next(obj)) {
            auto raddr = (struct rtnl_addr *)obj;
            auto local = rtnl_addr_get_local(raddr);

            if (rtnl_addr_get_ifindex(raddr) == dev.Index &&
                    local && nl_addr_cmp_prefix(local, addr.Addr) == 0) {
                reachable = true;
                break;
            }
        }

        /* Add proxy entry only if address is directly reachable */
        if (reachable) {
            error = Nl->ProxyNeighbour(dev.Index, addr, true);
            if (error)
                break;
        }
    }

    nl_cache_free(cache);

    return error;
}

TError TNetwork::DelAnnounce(const TNlAddr &addr) {
    TError error;

    for (auto &dev: Devices)
        error = Nl->ProxyNeighbour(dev.Index, addr, false);

    return error;
}

TError TNetwork::GetNatAddress(std::vector<TNlAddr> &addrs) {
    TError error;
    int offset;

    error = NatBitmap.Get(offset);
    if (error)
        return TError(error, "Cannot allocate NAT address");

    if (!NatBaseV4.IsEmpty()) {
        TNlAddr addr = NatBaseV4;
        addr.AddOffset(offset);
        addrs.push_back(addr);
    }

    if (!NatBaseV6.IsEmpty()) {
        TNlAddr addr = NatBaseV6;
        addr.AddOffset(offset);
        addrs.push_back(addr);
    }

    return TError::Success();
}

TError TNetwork::PutNatAddress(const std::vector<TNlAddr> &addrs) {

    for (auto &addr: addrs) {
        if (addr.Family() == AF_INET && !NatBaseV4.IsEmpty()) {
            uint64_t offset =  addr.GetOffset(NatBaseV4);
            return NatBitmap.Put(offset);
        }
        if (addr.Family() == AF_INET6 && !NatBaseV6.IsEmpty()) {
            uint64_t offset =  addr.GetOffset(NatBaseV6);
            return NatBitmap.Put(offset);
        }
    }

    return TError::Success();
}

std::string TNetwork::NewDeviceName(const std::string &prefix) {
    for (int retry = 0; retry < 100; retry++) {
        std::string name = prefix + std::to_string(IfaceName++);
        TNlLink link(Nl, name);
        if (link.Load())
            return name;
    }
    return prefix + "0";
}

std::string TNetwork::MatchDevice(const std::string &pattern) {
    for (auto &dev: Devices) {
        if (StringMatch(dev.Name, pattern))
            return dev.Name;
    }
    return pattern;
}

TError TNetwork::CreateTC(uint32_t handle, uint32_t parent, uint32_t leaf,
                          TUintMap &prio, TUintMap &rate, TUintMap &ceil) {
    TError error, result;
    TNlClass cls;

    for (auto &dev: Devices) {
        if (!dev.Managed || !dev.Prepared)
            continue;

        cls.Parent = parent;
        cls.Handle = handle;

        if (handle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID))
            cls.defRate = dev.Rate;
        else if (handle == TC_HANDLE(ROOT_TC_MAJOR, LEGACY_CONTAINER_ID))
            cls.defRate = dev.GetConfig(PortoRate);
        else
            cls.defRate = dev.GetConfig(ContainerRate);

        cls.Index = dev.Index;
        cls.Kind = dev.GetConfig(DeviceQdisc);
        cls.Prio = dev.GetConfig(prio);
        cls.Rate = dev.GetConfig(rate);
        cls.Ceil = dev.GetConfig(ceil);
        cls.Quantum = dev.GetConfig(DeviceQuantum, dev.MTU * 2);
        cls.RateBurst = dev.GetConfig(DeviceRateBurst, dev.MTU * 10);
        cls.CeilBurst = dev.GetConfig(DeviceCeilBurst, dev.MTU * 10);

        error = cls.Create(*Nl);
        if (error) {
            (void)cls.Delete(*Nl);
            error = cls.Create(*Nl);
        }
        if (error) {
            L_WRN("Cannot add tc class: {}", error);
            if (!result)
                result = error;
        }

        if (!error && leaf) {
            TNlQdisc ctq(dev.Index, leaf,
                         TC_HANDLE(TC_H_MIN(handle), CONTAINER_TC_MINOR));

            cls.Parent = handle;
            cls.Handle = leaf;

            if (leaf == TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR)) {
                cls.Rate = dev.GetConfig(DefaultRate);
                cls.defRate = cls.Rate;
                cls.Ceil = 0;

                ctq.Handle = TC_HANDLE(DEFAULT_TC_MAJOR, ROOT_TC_MINOR);
                ctq.Kind = dev.GetConfig(DefaultQdisc);
                ctq.Limit = dev.GetConfig(DefaultQdiscLimit);
                ctq.Quantum = dev.GetConfig(DefaultQdiscQuantum, dev.MTU * 2);
            } else {
                cls.Ceil = 0;

                ctq.Kind = dev.GetConfig(ContainerQdisc);
                ctq.Limit = dev.GetConfig(ContainerQdiscLimit, dev.MTU * 20);
                ctq.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 2);
            }

            error = cls.Create(*Nl);
            if (error) {
                L_WRN("Cannot add leaf tc class: {}", error);
                if (!result)
                    result = error;
            }

            error = ctq.Create(*Nl);
            if (error) {
                (void)ctq.Delete(*Nl);
                error = ctq.Create(*Nl);
            }
            if (error) {
                L_WRN("Cannot add container tc qdisc: {}", error);
                if (!result)
                    result = error;
            }
        }
    }

    return result;
}

TError TNetwork::DestroyTC(uint32_t handle, uint32_t leaf) {
    TError error, result;

    for (auto &dev: Devices) {
        if (!dev.Managed || !dev.Prepared)
            continue;

        TNlQdisc ctq(dev.Index, handle,
                     TC_HANDLE(TC_H_MIN(handle), CONTAINER_TC_MINOR));
        (void)ctq.Delete(*Nl);

        if (leaf) {
            TNlClass cls(dev.Index, TC_H_UNSPEC, leaf);
            (void)cls.Delete(*Nl);
        }

        TNlClass cls(dev.Index, TC_H_UNSPEC, handle);
        error = cls.Delete(*Nl);
        if (error) {
            L_WRN("Cannot del tc class: {}", error);
            if (!result)
                result = error;
        }
    }

    return result;
}

void TNetCfg::Reset() {
    /* default - create new empty netns */
    NewNetNs = true;
    Inherited = false;
    L3Only = true;
    Steal.clear();
    MacVlan.clear();
    IpVlan.clear();
    Veth.clear();
    L3lan.clear();
    IpIp6.clear();
    NetNsName = "";
    NetCtName = "";
}

TError TNetCfg::ParseNet(TMultiTuple &net_settings) {
    bool none = false;
    int idx = 0;

    Reset();

    if (net_settings.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

    for (auto &settings : net_settings) {
        TError error;

        if (settings.size() == 0)
            return TError(EError::InvalidValue, "Invalid net in: " +
                          MergeEscapeStrings(settings, 0));

        std::string type = StringTrim(settings[0]);

        if (type == "host" && settings.size() == 1)
            type = "inherited";

        if (type == "none") {
            none = true;
        } else if (type == "inherited") {
            NewNetNs = false;
            Inherited = true;
        } else if (type == "steal" || type == "host" /* legacy */) {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid net in: " +
                              MergeEscapeStrings(settings, 0));

            L3Only = false;
            Steal.push_back(StringTrim(settings[1]));
        } else if (type == "container") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid net in: " +
                              MergeEscapeStrings(settings, 0));
            NewNetNs = false;
            L3Only = false;
            NetCtName = StringTrim(settings[1]);
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid macvlan in: " +
                              MergeEscapeStrings(settings, 0));

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string type = "bridge";
            std::string hw = "";
            int mtu = -1;

            if (settings.size() > 3) {
                type = StringTrim(settings[3]);
                if (!TNlLink::ValidMacVlanType(type))
                    return TError(EError::InvalidValue,
                            "Invalid macvlan type " + type);
            }

            if (settings.size() > 4) {
                TError error = StringToInt(settings[4], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid macvlan mtu " + settings[4]);
            }

            if (settings.size() > 5) {
                hw = StringTrim(settings[5]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                            "Invalid macvlan address " + hw);
            }

            TMacVlanNetCfg mvlan;
            mvlan.Master = master;
            mvlan.Name = name;
            mvlan.Type = type;
            mvlan.Hw = hw;
            mvlan.Mtu = mtu;

            L3Only = false;
            MacVlan.push_back(mvlan);
        } else if (type == "ipvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid ipvlan in: " +
                              MergeEscapeStrings(settings, 0));

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string mode = "l2";
            int mtu = -1;

            if (settings.size() > 3) {
                mode = StringTrim(settings[3]);
                if (!TNlLink::ValidIpVlanMode(mode))
                    return TError(EError::InvalidValue,
                            "Invalid ipvlan mode " + mode);
            }

            if (settings.size() > 4) {
                TError error = StringToInt(settings[4], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid ipvlan mtu " + settings[4]);
            }

            TIpVlanNetCfg ipvlan;
            ipvlan.Master = master;
            ipvlan.Name = name;
            ipvlan.Mode = mode;
            ipvlan.Mtu = mtu;

            L3Only = false;
            IpVlan.push_back(ipvlan);
        } else if (type == "veth") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid veth in: " +
                              MergeEscapeStrings(settings, ' '));

            std::string name = StringTrim(settings[1]);
            std::string bridge = StringTrim(settings[2]);
            std::string hw = "";
            int mtu = -1;

            if (settings.size() > 3) {
                TError error = StringToInt(settings[3], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid veth mtu " + settings[3]);
            }

            if (settings.size() > 4) {
                hw = StringTrim(settings[4]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                            "Invalid veth address " + hw);
            }

            TVethNetCfg veth;
            veth.Bridge = bridge;
            veth.Name = name;
            veth.Hw = hw;
            veth.Mtu = mtu;
            veth.Peer = "portove-" + std::to_string(Id) + "-" + std::to_string(idx++);

            L3Only = false;
            Veth.push_back(veth);

        } else if (type == "L3") {
            TL3NetCfg l3;

            l3.Name = "eth0";
            l3.Nat = false;
            if (settings.size() > 1)
                l3.Name = StringTrim(settings[1]);

            l3.Mtu = -1;
            if (settings.size() > 2)
                l3.Master = StringTrim(settings[2]);

            L3lan.push_back(l3);

        } else if (type == "ipip6") {
            TIpIp6NetCfg ipip6;

            if (settings.size() != 4)
                return TError(EError::InvalidValue, "Invalid ipip6 in: " +
                              MergeEscapeStrings(settings, ' '));

            ipip6.Name = StringTrim(settings[1]);

            if (ipip6.Name == "ip6tnl0")
                return TError(EError::InvalidValue,
                              "Cannot modify default fallback tunnel");

            error = ipip6.Remote.Parse(AF_INET6, settings[2]);
            if (error)
                return error;

            error = ipip6.Local.Parse(AF_INET6, settings[3]);
            if (error)
                return error;

            /* As in net/ipv6/ip6_tunnel.c,
               we do not set IP6_TNL_F_IGN_ENCAP_LIMIT */

            ipip6.Mtu = ETH_DATA_LEN - sizeof(struct ip6_hdr) - 8;

            ipip6.Ttl = config().network().ipip6_ttl();
            ipip6.EncapLimit = config().network().ipip6_encap_limit();
            ipip6.DefaultRoute = false;

            IpIp6.push_back(ipip6);

        } else if (type == "NAT") {
            TL3NetCfg nat;

            nat.Nat = true;
            nat.Name = "eth0";
            nat.Mtu = -1;

            if (settings.size() > 1)
                nat.Name = StringTrim(settings[1]);

            L3lan.push_back(nat);

        } else if (type == "MTU") {
            if (settings.size() != 3)
                return TError(EError::InvalidValue, "Invalid MTU in: " +
                              MergeEscapeStrings(settings, 0));

            int mtu;
            TError error = StringToInt(settings[2], mtu);
            if (error)
                return error;

            for (auto &link: L3lan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: Veth) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: MacVlan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: IpVlan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: IpIp6) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            return TError(EError::InvalidValue, "Link not found: " + settings[1]);

        } else if (type == "autoconf") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid autoconf in: " +
                              MergeEscapeStrings(settings, 0));
            Autoconf.push_back(StringTrim(settings[1]));
        } else if (type == "netns") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid netns in: " +
                              MergeEscapeStrings(settings, 0));
            std::string name = StringTrim(settings[1]);
            TPath path("/var/run/netns/" + name);
            if (!path.Exists())
                return TError(EError::InvalidValue, "net namespace not found: " + name);
            NewNetNs = false;
            L3Only = false;
            NetNsName = name;
        } else {
            return TError(EError::InvalidValue, "Configuration is not specified");
        }
    }

    int single = none + Inherited;
    int mixed = Steal.size() + MacVlan.size() + IpVlan.size() + Veth.size() +
                L3lan.size() + IpIp6.size();

    if (single > 1 || (single == 1 && mixed))
        return TError(EError::InvalidValue, "none/host/inherited can't be mixed with other types");

    return TError::Success();
}

TError TNetCfg::ParseIp(TMultiTuple &ip_settings) {
    IpVec.clear();
    for (auto &settings : ip_settings) {
        TError error;

        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid ip address/prefix in: " +
                          MergeEscapeStrings(settings, ' '));

        TIpVec ip;
        ip.Iface = settings[0];
        error = ip.Addr.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        IpVec.push_back(ip);

        for (auto &l3: L3lan) {
            if (l3.Name == ip.Iface) {
                if (!ip.Addr.IsHost())
                    return TError(EError::InvalidValue, "Invalid ip prefix for L3 network");
                l3.Addrs.push_back(ip.Addr);
            }
        }

        for (auto &ipip6: IpIp6) {
            if (ipip6.Name == ip.Iface)
                ipip6.DefaultRoute = ip.Addr.IsHost();
        }
    }
    return TError::Success();
}

void TNetCfg::FormatIp(TMultiTuple &ip_settings) {
    for (auto &ip: IpVec)
        ip_settings.push_back({ ip.Iface , ip.Addr.Format() });
}

TError TNetCfg::ParseGw(TMultiTuple &gw_settings) {
    GwVec.clear();
    for (auto &settings : gw_settings) {
        TError error;

        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid gateway address/prefix in: " +
                          MergeEscapeStrings(settings, ' '));

        TGwVec gw;
        gw.Iface = settings[0];
        error = gw.Addr.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        GwVec.push_back(gw);
    }
    return TError::Success();
}

std::string TNetCfg::GenerateHw(const std::string &name) {
    uint32_t n = Crc32(name);
    uint32_t h = Crc32(Hostname);

    return StringFormat("02:%02x:%02x:%02x:%02x:%02x",
            (n & 0x000000FF) >> 0,
            (h & 0xFF000000) >> 24,
            (h & 0x00FF0000) >> 16,
            (h & 0x0000FF00) >> 8,
            (h & 0x000000FF) >> 0);
}

TError TNetCfg::ConfigureVeth(TVethNetCfg &veth) {
    auto parentNl = ParentNet->GetNl();
    TNlLink peer(parentNl, ParentNet->NewDeviceName("portove-"));
    TError error;

    std::string hw = veth.Hw;
    if (hw.empty() && !Hostname.empty())
        hw = GenerateHw(veth.Name + veth.Peer);

    error = peer.AddVeth(veth.Name, hw, veth.Mtu, 0, NetNs.GetFd());
    if (error)
        return error;

    if (!veth.Bridge.empty()) {
        TNlLink bridge(parentNl, veth.Bridge);
        error = bridge.Load();
        if (error)
            return error;

        error = bridge.Enslave(peer.GetName());
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetCfg::ConfigureL3(TL3NetCfg &l3) {
    auto lock = HostNetwork->ScopedLock();
    std::string peerName = HostNetwork->NewDeviceName("L3-");
    auto parentNl = HostNetwork->GetNl();
    auto Nl = Net->GetNl();
    TNlLink peer(parentNl, peerName);
    TNlAddr gate4, gate6;
    TError error;

    if (l3.Nat && l3.Addrs.empty()) {
        error = HostNetwork->GetNatAddress(l3.Addrs);
        if (error)
            return error;

        for (auto &addr: l3.Addrs) {
            TIpVec ip;
            ip.Iface = l3.Name;
            ip.Addr = addr;
            IpVec.push_back(ip);
        }

        SaveIp = true;
    }

    error = HostNetwork->GetGateAddress(l3.Addrs, gate4, gate6, l3.Mtu, l3.Group);
    if (error)
        return error;

    for (auto &addr : l3.Addrs) {
        if (addr.Family() == AF_INET && gate4.IsEmpty())
            return TError(EError::InvalidValue, "Ipv4 gateway not found");
        if (addr.Family() == AF_INET6 && gate6.IsEmpty())
            return TError(EError::InvalidValue, "Ipv6 gateway not found");
    }

    error = peer.AddVeth(l3.Name, "", l3.Mtu, l3.Group, NetNs.GetFd());
    if (error)
        return error;

    TNlLink link(Nl, l3.Name);
    error = link.Load();
    if (error)
        return error;

    error = link.Up();
    if (error)
        return error;

    auto peerAddr = peer.GetAddr();

    if (!gate4.IsEmpty()) {
        error = Nl->PermanentNeighbour(link.GetIndex(), gate4, peerAddr, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate4);
        if (error)
            return error;
        error = link.SetDefaultGw(gate4);
        if (error)
            return error;
    }

    if (!gate6.IsEmpty()) {
        error = Nl->PermanentNeighbour(link.GetIndex(), gate6, peerAddr, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate6);
        if (error)
            return error;
        error = link.SetDefaultGw(gate6);
        if (error)
            return error;
    }

    for (auto &addr : l3.Addrs) {
        error = peer.AddDirectRoute(addr);
        if (error)
            return error;

        if (config().network().proxy_ndp()) {
            error = HostNetwork->AddAnnounce(addr, HostNetwork->MatchDevice(l3.Master));
            if (error)
                return error;
        }
    }

    return TError::Success();
}

TError TNetCfg::ConfigureInterfaces() {
    std::vector<std::string> links;
    auto parent_lock = ParentNet->ScopedLock();
    auto source_nl = ParentNet->GetNl();
    auto target_nl = Net->GetNl();
    TError error;

    for (auto &dev : Steal) {
        TNlLink link(source_nl, dev);
        error = link.ChangeNs(dev, NetNs.GetFd());
        if (error)
            return error;
        links.emplace_back(dev);
    }

    for (auto &ipvlan : IpVlan) {
        std::string master = ParentNet->MatchDevice(ipvlan.Master);

        TNlLink link(source_nl, "piv" + std::to_string(GetTid()));
        error = link.AddIpVlan(master, ipvlan.Mode, ipvlan.Mtu);
        if (error)
            return error;

        error = link.ChangeNs(ipvlan.Name, NetNs.GetFd());
        if (error) {
            (void)link.Remove();
            return error;
        }
        links.emplace_back(ipvlan.Name);
    }

    for (auto &mvlan : MacVlan) {
        std::string master = ParentNet->MatchDevice(mvlan.Master);

        std::string hw = mvlan.Hw;
        if (hw.empty() && !Hostname.empty())
            hw = GenerateHw(master + mvlan.Name);

        TNlLink link(source_nl, "pmv" + std::to_string(GetTid()));
        error = link.AddMacVlan(master, mvlan.Type, hw, mvlan.Mtu);
        if (error)
                return error;

        error = link.ChangeNs(mvlan.Name, NetNs.GetFd());
        if (error) {
            (void)link.Remove();
            return error;
        }
        links.emplace_back(mvlan.Name);
    }

    for (auto &veth : Veth) {
        error = ConfigureVeth(veth);
        if (error)
            return error;
        links.emplace_back(veth.Name);
    }

    parent_lock.unlock();

    for (auto &l3 : L3lan) {
        error = ConfigureL3(l3);
        if (error)
            return error;
        links.emplace_back(l3.Name);
    }

    for (auto &ipip6: IpIp6) {
        TNlLink link(Net->GetNl(), ipip6.Name);

        error = link.AddIp6Tnl(ipip6.Name, ipip6.Remote, ipip6.Local,
                               IPPROTO_IPIP, ipip6.Mtu, ipip6.EncapLimit,
                               ipip6.Ttl);

        if (error)
            return error;

        links.emplace_back(ipip6.Name);
    }

    TNlLink loopback(target_nl, "lo");
    error = loopback.Load();
    if (error)
        return error;
    error = loopback.Up();
    if (error)
        return error;

    Net->ManagedNamespace = true;

    error = Net->RefreshDevices();
    if (error)
        return error;

    Net->NewManagedDevices = false;

    for (auto &name: links) {
        if (!Net->DeviceIndex(name))
            return TError(EError::Unknown, "network device " + name + " not found");
    }

    for (auto &dev: Net->Devices) {
        if (!NetUp) {
            bool found = false;
            for (auto &ip: IpVec)
                if (ip.Iface == dev.Name)
                    found = true;
            for (auto &gw: GwVec)
                if (gw.Iface == dev.Name)
                    found = true;
            for (auto &ac: Autoconf)
                if (ac == dev.Name)
                    found = true;
            if (!found)
                continue;
        }

        TNlLink link(target_nl, dev.Name);
        error = link.Load();
        if (error)
            return error;
        error = link.Up();
        if (error)
            return error;

        for (auto &ip: IpVec) {
            if (ip.Iface == dev.Name) {
                error = link.AddAddress(ip.Addr);
                if (error)
                    return error;
            }
        }

        for (auto &gw: GwVec) {
            if (gw.Iface == dev.Name) {
                error = link.SetDefaultGw(gw.Addr);
                if (error)
                    return error;
            }
        }

    }

    for (auto &ipip6: IpIp6) {
        if (ipip6.DefaultRoute) {
            TNlLink link(target_nl, ipip6.Name);
            error = link.Load();
            if (error)
                return error;

            TNlAddr addr;
            addr.Parse(AF_INET, "default");

            error = link.AddDirectRoute(addr);
            if (error)
                return error;
        }
    }

    return TError::Success();
}

TError TNetCfg::PrepareNetwork() {
    TError error;

    if (NewNetNs && L3Only && config().network().l3_migration_hack() &&
            L3lan.size() && L3lan[0].Addrs.size()) {
        auto lock = LockContainers();

        for (auto &it: Containers) {
            auto &ct = it.second;
            if (!ct->Net || ct->IpList.empty())
                continue;

            for (auto cfg: ct->IpList) {
                TNlAddr addr;
                if (cfg.size() == 2 && !addr.Parse(AF_UNSPEC, cfg[1]) &&
                        addr.IsMatch(L3lan[0].Addrs[0]) &&
                        !ct->OpenNetns(NetNs)) {
                    L_ACT("Reuse L3 addr {} network {}", addr.Format(), ct->Name);
                    Net = ct->Net;
                    lock.unlock();
                    auto net_lock = Net->ScopedLock();
                    Net->Owners++;
                    return TError::Success();
                }
            }
        }
    }

    if (NewNetNs) {
        Net = std::make_shared<TNetwork>();
        error = Net->ConnectNew(NetNs);
        if (error)
            return error;

        error = ConfigureInterfaces();
        if (error) {
            (void)DestroyNetwork();
            return error;
        }

        TNetwork::AddNetwork(NetNs.GetInode(), Net);
    } else if (!Parent) {
        Net = std::make_shared<TNetwork>();
        error = Net->Connect();
        if (error)
            return error;

        error = NetNs.Open(GetTid(), "ns/net");
        if (error)
            return error;

        error = Net->RefreshDevices();
        if (error)
            return error;
        Net->NewManagedDevices = false;

        if (config().network().has_nat_first_ipv4())
            Net->NatBaseV4.Parse(AF_INET, config().network().nat_first_ipv4());
        if (config().network().has_nat_first_ipv6())
            Net->NatBaseV6.Parse(AF_INET6, config().network().nat_first_ipv6());
        if (config().network().has_nat_count())
            Net->NatBitmap.Resize(config().network().nat_count());

        HostNetwork = Net;
        TNetwork::AddNetwork(NetNs.GetInode(), Net);
        NetWorker.Start();
    } else if (Inherited) {
        Net = Parent->Net;
        error = Parent->OpenNetns(NetNs);
        if (error)
            return error;
    } else if (NetNsName != "") {
        error = NetNs.Open("/var/run/netns/" + NetNsName);
        if (error)
            return error;

        Net = TNetwork::GetNetwork(NetNs.GetInode());
        if (!Net) {
            Net = std::make_shared<TNetwork>();

            error = Net->ConnectNetns(NetNs);
            if (error)
                return error;

            error = Net->RefreshDevices();
            if (error)
                return error;
            Net->NewManagedDevices = false;

            TNetwork::AddNetwork(NetNs.GetInode(), Net);
        }
    } else if (NetCtName != "") {
        if (!CL)
            return TError(EError::Unknown, "No client for net container");

        std::shared_ptr<TContainer> target;
        auto lock = LockContainers();
        error = CL->ResolveContainer(NetCtName, target);
        if (error)
            return error;

        error = CL->CanControl(*target);
        if (error)
            return TError(error, "net container " + NetCtName);

        error = target->OpenNetns(NetNs);
        if (error)
            return error;

        Net = target->Net;
    }

    return TError::Success();
}

TError TNetCfg::DestroyNetwork() {
    TError error;

    for (auto &l3 : L3lan) {
        auto lock = HostNetwork->ScopedLock();
        if (config().network().proxy_ndp()) {
            for (auto &addr : l3.Addrs) {
                error = HostNetwork->DelAnnounce(addr);
                if (error)
                    L_ERR("Cannot remove announce {} : {}", addr.Format(), error);
            }
        }
        if (l3.Nat) {
            error = HostNetwork->PutNatAddress(l3.Addrs);
            if (error)
                L_ERR("Cannot put NAT address : {}", error);

            auto ip = IpVec.begin();
            while (ip != IpVec.end()) {
                if (ip->Iface == l3.Name)
                    ip = IpVec.erase(ip);
                else
                    ++ip;
            }
            SaveIp = true;
        }
    }

    return error;
}

struct TClsStats {
    uint32_t Parent = 0;
    TNetStats Stats;
};

typedef std::map<uint32_t, TClsStats> TDevClsStats;
typedef std::map<int, TDevClsStats> TDevTcStats;

void TNetwork::RefreshStats(std::list<std::shared_ptr<TContainer>> &subtree) {
    TDevTcStats tc_stats;

    for (auto &dev: Devices) {
        if (!dev.Managed || !dev.Prepared)
            continue;

        struct nl_cache *cache;

        int ret = rtnl_class_alloc_cache(GetSock(), dev.Index, &cache);

        if (ret) {
            L_WRN("Failed to retrieve net statistics for dev {} : {}",
                  dev.Index, Nl->Error(ret, "Cannot fill class cache"));

            NeedRefresh = true;
            continue;
        }

        TDevClsStats &dev_tc_stats = tc_stats[dev.Index];

        for (auto obj = nl_cache_get_first(cache); obj;
             obj = nl_cache_get_next(obj)) {

            struct rtnl_class *cls = (struct rtnl_class *)obj;
            auto parent = rtnl_tc_get_parent(TC_CAST(cls));
            auto handle = rtnl_tc_get_handle(TC_CAST(cls));

            dev_tc_stats[handle].Parent = parent;
            TNetStats &s = dev_tc_stats[handle].Stats;

            s.RxBytes = dev.Stats.RxBytes;
            s.TxBytes = dev.Stats.TxBytes;
            s.RxPackets = dev.Stats.RxPackets;
            s.TxPackets = dev.Stats.TxPackets;
            s.RxDrops = dev.Stats.RxDrops;
            s.TxDrops = dev.Stats.TxDrops;

            s.Packets += rtnl_tc_get_stat(TC_CAST(cls), RTNL_TC_PACKETS);
            s.Bytes += rtnl_tc_get_stat(TC_CAST(cls), RTNL_TC_BYTES);
            s.Drops += rtnl_tc_get_stat(TC_CAST(cls), RTNL_TC_DROPS);
            s.Overlimits += rtnl_tc_get_stat(TC_CAST(cls), RTNL_TC_OVERLIMITS);

            if (!strcmp(rtnl_tc_get_kind(TC_CAST(cls)), "hfsc")) {
                while (parent) {
                    TNetStats &p = dev_tc_stats[parent].Stats;

                    p.Packets += s.Packets;
                    p.Bytes += s.Bytes;
                    p.Drops += s.Drops;
                    p.Overlimits += s.Overlimits;

                    parent = dev_tc_stats[parent].Parent;
                }
            }
        }

        nl_cache_free(cache);
    }

    auto refresh_time = GetCurrentTimeMs();

    for (auto &ct : subtree) {
        auto lock = ct->LockNetState();
        auto handle = ct->ContainerTC;

        if (ct->Net.get() != this || ct->NetState)
            continue;

        auto &ct_stats = ct->NetStats;

        ct_stats.clear();

        for (auto &dev : Devices) {
            if (tc_stats[dev.Index].find(handle) != tc_stats[dev.Index].end()) {
                ct_stats[dev.Name] = tc_stats[dev.Index][handle].Stats;
            } else {
                L_WRN("Cannot find tc class {} at {}", handle, dev.GetDesc());
                NeedRefresh = true;
            }

            ct_stats["group " + dev.GroupName].RxBytes += ct_stats[dev.Name].RxBytes;
            ct_stats["group " + dev.GroupName].RxPackets += ct_stats[dev.Name].RxPackets;
            ct_stats["group " + dev.GroupName].RxDrops += ct_stats[dev.Name].RxDrops;

            ct_stats["group " + dev.GroupName].TxBytes += ct_stats[dev.Name].TxBytes;
            ct_stats["group " + dev.GroupName].TxPackets += ct_stats[dev.Name].TxPackets;
            ct_stats["group " + dev.GroupName].TxDrops += ct_stats[dev.Name].TxDrops;

            ct_stats["group " + dev.GroupName].Bytes += ct_stats[dev.Name].Bytes;
            ct_stats["group " + dev.GroupName].Packets += ct_stats[dev.Name].Packets;
            ct_stats["group " + dev.GroupName].Drops += ct_stats[dev.Name].Drops;
            ct_stats["group " + dev.GroupName].Overlimits += ct_stats[dev.Name].Overlimits;
        }

        ct->NetStatsRefreshTime = refresh_time;
    }
}

void TNetWorker::Start() {
    Shutdown = false;
    Thread = std::thread(&TNetWorker::Loop, this);
}

void TNetWorker::Stop() {
    auto lock = LockNetworks();
    Shutdown = true;
    lock.unlock();

    Cv.notify_all();

    Thread.join();
}

void TNetWorker::Wake() {
    auto lock = LockNetworks();

    WorkPending = true;
    lock.unlock();

    Cv.notify_all();
}

TError TNetwork::RefreshClasses(std::list<std::shared_ptr<TContainer>> &subtree) {
    TError ret;
    int try_count = 0;

    do {
        ret = TError::Success();

        for (auto &ct : subtree) {
            auto lock = ct->LockNetState();
            TUintMap prio = ct->NetPriority;
            TUintMap limit = ct->NetLimit;
            TUintMap rx_limit = ct->NetRxLimit;
            TUintMap guarantee = ct->NetGuarantee;
            uint32_t ct_tc = ct->ContainerTC;
            uint32_t parent_tc = ct->ParentTC;
            uint32_t leaf_tc = ct->LeafTC;
            auto net = ct->Net;
            auto state = ct->NetState;
            lock.unlock();

            if (!net || net.get() != this && HostNetwork.get() != this ||
                !NeedRefresh && !state)
                continue;

            TError err = CreateTC(ct_tc, parent_tc, leaf_tc,
                                  prio, guarantee, limit);

            if (!err && this != HostNetwork.get() && rx_limit.size() > 0)
                err = net->CreateIngressQdisc(rx_limit);

            lock.lock();
            if (net.get() == this || err)
                ct->NetState = err;
            lock.unlock();

            if (err) {
                L_ACT("Perform net {} reset", try_count ? "hard" : "soft");

                RefreshDevices(try_count);
                NewManagedDevices = false;
                NeedRefresh = true;
                ret = err;

                break;
            }
        }
    } while (ret && try_count++ < 2);

    if (!ret)
        NeedRefresh = false;

    return ret;
}

void TNetWorker::Loop() {
    SetProcessName(PORTOD_NETWORKER_NAME);
    PORTO_ASSERT(RootContainer && HostNetwork);

    uint64_t deadline = 0lu;
    auto lock = LockNetworks();

    while (!Shutdown) {
        auto now = GetCurrentTimeMs();

        while (!Shutdown && !WorkPending && now < deadline) {
            Cv.wait_for(lock, std::chrono::milliseconds(deadline - now));
            now = GetCurrentTimeMs();
        }

        auto nets = Networks;
        auto work_pending = false;
        auto stats_needed = StatsNeeded;

        WorkPending = false;

        lock.unlock();

        auto ct_tree = RootContainer->Subtree();
        ct_tree.reverse();

        if (now >= deadline || stats_needed) {
            for (auto &it : nets) {
                auto net = it.second.lock();
                if (!net)
                    continue;

                auto net_lock = net->ScopedLock();

                net->RefreshDevices();
                if (net->NewManagedDevices) {
                    net->NewManagedDevices = false;
                    net->NeedRefresh = true;
                }
            }
        }

        auto host_lock = HostNetwork->ScopedLock();

        if (HostNetwork->RefreshClasses(ct_tree)) {
            L("Failed to refresh host net, try next time");
            work_pending = true;
        }

        for (auto &ct : ct_tree) {
            auto lock = ct->LockNetState();

            if (ct->Net == HostNetwork)
                ct->NetCv.notify_all();
        }

        host_lock.unlock();

        for (auto &it : nets) {
            auto net = it.second.lock();
            if (!net)
                continue;

            auto net_lock = net->ScopedLock();

            if (net != HostNetwork) {
                if (net->RefreshClasses(ct_tree)) {
                    L("Failed to refresh host net, try next time");
                    work_pending = true;
                }

                for (auto &ct : ct_tree) {
                    auto lock = ct->LockNetState();

                    if (ct->Net == net)
                        ct->NetCv.notify_all();
                }
            }

            if (stats_needed) {
                net->RefreshStats(ct_tree);

                lock.lock();
                StatsNeeded = false;
                lock.unlock();

                Cv.notify_all();
            }
        }

        deadline = GetCurrentTimeMs() + NetworkRefreshPeriod;

        lock.lock();
        WorkPending |= work_pending;
    }
}

TError TNetWorker::RefreshNetwork(std::shared_ptr<TContainer> ct) {
    auto lock = ct->LockNetState();

    ct->NetState = TError::Queued();

    NetWorker.Wake();

    while (ct->NetState.GetError() == EError::Queued)
        ct->NetCv.wait(lock);

    if (ct->NetState)
        return ct->NetState;

    return TError::Success();
}

void TNetWorker::RefreshStats(std::shared_ptr<TContainer> ct, bool force) {
    auto start = GetCurrentTimeMs();
    auto ct_lock = ct->LockNetState();

    if (!force && start < ct->NetStatsRefreshTime + NetworkRefreshPeriod)
        return;

    while (ct->NetStatsRefreshTime < start) {
        auto lock = LockNetworks();
        if (!StatsNeeded) {
            StatsNeeded = true;
            WorkPending = true;
            Cv.notify_all();
        }

        ct_lock.unlock();
        Cv.wait(lock);
        ct_lock.lock();
    }
}
