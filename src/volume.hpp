#pragma once

#include <memory>
#include <string>
#include <set>
#include "common.hpp"
#include "statistics.hpp"
#include "util/path.hpp"
#include "util/locks.hpp"

constexpr const char *V_PATH = "path";
constexpr const char *V_BACKEND = "backend";
constexpr const char *V_READY = "ready";
constexpr const char *V_PRIVATE = "private";

constexpr const char *V_ID = "_id";
constexpr const char *V_RAW_CONTAINERS = "_containers";
constexpr const char *V_CONTAINERS = "containers";
constexpr const char *V_LOOP_DEV = "_loop_dev";
constexpr const char *V_AUTO_PATH = "_auto_path";

constexpr const char *V_OWNER_USER = "owner_user";
constexpr const char *V_OWNER_GROUP = "owner_group";
constexpr const char *V_CREATOR = "creator";

constexpr const char *V_USER = "user";
constexpr const char *V_GROUP = "group";
constexpr const char *V_PERMISSIONS = "permissions";

constexpr const char *V_STORAGE = "storage";
constexpr const char *V_LAYERS = "layers";
constexpr const char *V_READ_ONLY = "read_only";

constexpr const char *V_SPACE_LIMIT = "space_limit";
constexpr const char *V_INODE_LIMIT = "inode_limit";
constexpr const char *V_SPACE_GUARANTEE = "space_guarantee";
constexpr const char *V_INODE_GUARANTEE = "inode_guarantee";

constexpr const char *V_SPACE_USED = "space_used";
constexpr const char *V_INODE_USED = "inode_used";
constexpr const char *V_SPACE_AVAILABLE = "space_available";
constexpr const char *V_INODE_AVAILABLE = "inode_available";

constexpr const char *V_PLACE = "place";

class TVolume;
class TContainer;
class TKeyValue;

class TVolumeBackend {
public:
    TVolume *Volume;
    virtual TError Configure(void);
    virtual TError Save(void);
    virtual TError Restore(void);
    virtual TError Build(void) =0;
    virtual TError Destroy(void) =0;
    virtual TError StatFS(TStatFS &result) =0;
    virtual TError Resize(uint64_t space_limit, uint64_t inode_limit);
};

class TVolume : public std::enable_shared_from_this<TVolume>,
                public TLockable,
                public TNonCopyable {

    std::unique_ptr<TVolumeBackend> Backend;
    TError OpenBackend();

public:
    TPath Path;
    TPath InternalPath;
    bool IsAutoPath = false;

    TPath Place = PORTO_PLACE;
    bool CustomPlace = false;

    std::string Storage;
    TPath StoragePath;
    TFile StorageFd; /* during build */
    bool KeepStorage = false;

    std::string BackendType;
    std::string Id;
    bool IsReady = false;
    bool IsDying = false;

    int Device = -1;
    bool IsReadOnly = false;

    std::vector<std::string> Layers;
    std::vector<std::string> Containers;

    uint64_t SpaceLimit = 0;
    uint64_t SpaceGuarantee = 0;
    uint64_t InodeLimit = 0;
    uint64_t InodeGuarantee = 0;

    TCred VolumeOwner;

    TCred VolumeCred;
    unsigned VolumePerms = 0;

    std::string Creator;
    std::string Private;

    std::set<std::shared_ptr<TVolume>> Nested;

    TVolume() {
        Statistics->VolumesCount++;
    }
    ~TVolume() {
        Statistics->VolumesCount--;
    }

    static TError Create(const TStringMap &cfg,
                         std::shared_ptr<TVolume> &volume);

    static std::shared_ptr<TVolume> Find(const TPath &path);
    static TError Find(const TPath &path, std::shared_ptr<TVolume> &volume);
    static std::shared_ptr<TVolume> Locate(const TPath &path);

    TError Configure(const TStringMap &cfg);
    TError ApplyConfig(const TStringMap &cfg);
    TStringMap DumpConfig(const TPath &root);

    TError DependsOn(const TPath &path);
    TError CheckDependencies();

    TError Build(void);

    static void DestroyAll();
    TError DestroyOne(bool strict = false);
    TError Destroy(bool strict = false);

    TError Save(void);
    TError Restore(const TKeyValue &node);

    static void RestoreAll(void);

    TError LinkContainer(TContainer &container);
    TError UnlinkContainer(TContainer &container, bool strict = false);

    TPath GetInternal(const std::string &type) const;
    unsigned long GetMountFlags(void) const;

    TError Tune(const TStringMap &cfg);

    TError CheckGuarantee(uint64_t space_guarantee, uint64_t inode_guarantee) const;

    bool HaveQuota(void) const {
        return SpaceLimit || InodeLimit;
    }

    bool HaveStorage(void) const {
        return !Storage.empty();
    }

    /* User provides directory for storage */
    bool UserStorage(void) const {
        return Storage[0] == '/';
    }

    /* They do not keep data in StoragePath */
    bool RemoteStorage(void) const {
        return BackendType == "rbd" ||
               BackendType == "lvm" ||
               BackendType == "tmpfs" ||
               BackendType == "quota";
    }

    /* Backend storage could be a regular file */
    bool FileStorage(void) const {
        return BackendType == "loop";
    }

    bool HaveLayers(void) const {
        return !Layers.empty();
    }

    TError StatFS(TStatFS &result) const;

    TError GetUpperLayer(TPath &upper);

    friend bool operator<(const std::shared_ptr<TVolume> &lhs,
                          const std::shared_ptr<TVolume> &rhs) {
        return lhs->Path < rhs->Path;
    }
};

struct TVolumeProperty {
    std::string Name;
    std::string Desc;
    bool ReadOnly;
};

extern std::vector<TVolumeProperty> VolumeProperties;

extern std::mutex VolumesMutex;
extern std::map<TPath, std::shared_ptr<TVolume>> Volumes;
extern TPath VolumesKV;

static inline std::unique_lock<std::mutex> LockVolumes() {
    return std::unique_lock<std::mutex>(VolumesMutex);
}

extern TError PutLoopDev(const int loopNr); /* Legacy */
