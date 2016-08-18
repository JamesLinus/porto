#pragma once

#include <string>
#include <functional>
#include <memory>

#include "common.hpp"
extern "C" {
#include <arpa/inet.h>
#include <linux/netlink.h>
}

struct nl_sock;
struct rtnl_link;
struct nl_cache;
struct nl_addr;
class TNlLink;

class TNlAddr {
public:
    struct nl_addr *Addr = nullptr;

    TNlAddr() {}
    TNlAddr(struct nl_addr *addr);
    TNlAddr(const TNlAddr &other);
    TNlAddr &operator=(const TNlAddr &other);
    ~TNlAddr();

    void Forget();

    TError Parse(int family, const std::string &string);
    std::string Format();

    int Family() const;
    bool IsEmpty() const;
    bool IsHost() const;

    void AddOffset(uint64_t offset);
    uint64_t GetOffset(const TNlAddr &base) const;
};

uint32_t TcHandle(uint16_t maj, uint16_t min);

class TNl : public std::enable_shared_from_this<TNl>,
            public TNonCopyable {
    struct nl_sock *Sock = nullptr;

public:

    TNl() {}
    ~TNl() { Disconnect(); }

    TError Connect();
    void Disconnect();

    struct nl_sock *GetSock() const { return Sock; }

    int GetFd();
    TError OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links, bool all);

    static TError Error(int nl_err, const std::string &desc);
    void Dump(const std::string &prefix, void *obj) const;
    void DumpCache(struct nl_cache *cache) const;

    TError ProxyNeighbour(int ifindex, const TNlAddr &addr, bool add);
};

class TNlLink : public TNonCopyable {
    std::shared_ptr<TNl> Nl;
    struct rtnl_link *Link = nullptr;

    TError AddXVlan(const std::string &vlantype,
                    const std::string &master,
                    uint32_t type,
                    const std::string &hw,
                    int mtu);

public:

    TNlLink(std::shared_ptr<TNl> sock, const std::string &name);
    TNlLink(std::shared_ptr<TNl> sock, struct rtnl_link *link);
    ~TNlLink();
    TError Load();

    int GetIndex() const;
    std::string GetName() const;
    std::string GetType() const;
    std::string GetDesc() const;
    bool IsLoopback() const;
    bool IsRunning() const;
    TError Error(int nl_err, const std::string &desc) const;
    void Dump(const std::string &prefix, void *obj = nullptr) const;

    TError Remove();
    TError Up();
    TError Enslave(const std::string &name);
    TError ChangeNs(const std::string &newName, int nsFd);
    TError AddIpVlan(const std::string &master,
                     const std::string &mode, int mtu);
    TError AddMacVlan(const std::string &master,
                      const std::string &type, const std::string &hw,
                      int mtu);
    TError AddVeth(const std::string &name, const std::string &hw, int mtu, int nsFd);

    static bool ValidIpVlanMode(const std::string &mode);
    static bool ValidMacVlanType(const std::string &type);
    static bool ValidMacAddr(const std::string &hw);

    TError AddDirectRoute(const TNlAddr &addr);
    TError SetDefaultGw(const TNlAddr &addr);
    TError AddAddress(const TNlAddr &addr);
    TError WaitAddress(int timeout_s);

    struct nl_sock *GetSock() const { return Nl->GetSock(); }
    std::shared_ptr<TNl> GetNl() { return Nl; };
};

class TNlClass : public TNonCopyable {
    const uint32_t Parent, Handle;

public:
    TNlClass(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}

    TError GetProperties(const TNlLink &link, uint32_t &prio, uint32_t &rate, uint32_t &ceil);
    bool Exists(const TNlLink &link);
};

class TNlQdisc {
public:
    const int Index;
    const uint32_t Parent, Handle;
    std::string Kind;
    uint32_t Default = 0;
    uint32_t Limit = 0;
    uint32_t Quantum = 0;
    TNlQdisc(int index, uint32_t parent, uint32_t handle) :
        Index(index), Parent(parent), Handle(handle) {}

    TError Create(const TNl &nl);
    TError Delete(const TNl &nl);
    bool Check(const TNl &nl);
};

class TNlCgFilter : public TNonCopyable {
    const int Index;
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";
    const uint32_t Parent, Handle;

public:
    TNlCgFilter(int index, uint32_t parent, uint32_t handle) :
        Index(index), Parent(parent), Handle(handle) {}
    TError Create(const TNl &nl);
    bool Exists(const TNl &nl);
    TError Delete(const TNl &nl);
};
