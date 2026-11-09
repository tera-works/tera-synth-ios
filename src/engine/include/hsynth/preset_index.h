// Preset browser index shared by the hosts: scans preset folders, extracts
// the metadata (name, category, tags, layer types, samples) and answers
// search queries. Favorites are kept in a small JSON file.
//
// Not real-time: use from a UI / loader thread. Not thread-safe (one owner).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace hsynth {

struct PresetInfo {
    std::string file;      // UTF-8 path
    std::string name;
    std::string category;  // "" = uncategorised
    std::string author;
    std::string comment;
    std::vector<std::string> tags;  // from the preset ("tags")
    std::vector<std::string> autoTags;  // derived: layer types, "sampled", "user"
    bool user = false;     // from a user folder (editable)
    bool favorite = false;
    std::string sortKey;   // lower-case name
};

struct PresetQuery {
    std::string text;          // words, all must match (name / category / tags / author / comment); "#tag" = tag only
    std::string category;      // "" = all
    bool favoritesOnly = false;
    bool userOnly = false;
};

class PresetIndex {
public:
    // Folders to scan (recursively); presets in `user` folders are editable.
    void setFolders(std::vector<std::pair<std::string, bool>> folders) { folders_ = std::move(folders); }
    void setFavoritesFile(const std::string& path);
    void rescan();

    const std::vector<PresetInfo>& all() const { return items_; }
    // Indices into all(), sorted by category then name.
    std::vector<int> query(const PresetQuery& q) const;
    // Categories with their preset counts (sorted, "" listed as "Uncategorised").
    std::vector<std::pair<std::string, int>> categories() const;
    // Every tag (preset + automatic) with its count, most used first.
    std::vector<std::pair<std::string, int>> tags() const;
    int find(const std::string& file) const;  // index or -1

    void setFavorite(int index, bool on);  // saved immediately
    // Rewrites category / tags of a user preset file (and the index entry).
    bool updateMetadata(int index, const std::string& category, const std::vector<std::string>& tags, std::string& error);

    // "a, b ,c" -> {"a","b","c"} (trimmed, empty ones dropped)
    static std::vector<std::string> splitTags(const std::string& s);
    static std::string joinTags(const std::vector<std::string>& t);

private:
    void saveFavorites() const;
    std::vector<std::pair<std::string, bool>> folders_;
    std::string favoritesFile_;
    std::set<std::string> favorites_;
    std::vector<PresetInfo> items_;
};

}  // namespace hsynth
