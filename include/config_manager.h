#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <mutex>

/**
 * @brief Represents a DNS profile containing server addresses and metadata.
 */
struct DnsProfile {
    std::string id;
    std::string name;
    std::string description;
    std::string primary_ipv4;
    std::string secondary_ipv4;
    std::string primary_ipv6;
    std::string secondary_ipv6;
    std::string doh_template;
    bool is_builtin = false;
    bool is_dhcp = false;
    bool is_smart_dns = false;
    std::string website_url;
    std::string unblocked_summary;
    std::vector<std::string> ip_pool;

    bool operator==(const DnsProfile& other) const = default;
};

/**
 * @brief Manages loading, saving, updating and querying DNS configuration profiles.
 */
class ConfigManager {
public:
    static constexpr size_t MAX_IP_POOL_SIZE = 64;
    static constexpr size_t MAX_CONFIG_FILE_BYTES = 4 * 1024 * 1024;

    /**
     * @brief Constructs ConfigManager.
     * @param config_path Optional custom configuration file path. If empty, uses %APPDATA%\QuickDNS\profiles.json.
     */
    explicit ConfigManager(const std::filesystem::path& config_path = {});

    /**
     * @brief Returns a copy of all loaded DNS profiles.
     */
    std::vector<DnsProfile> get_all_profiles() const;

    /**
     * @brief Finds a DNS profile by its unique ID.
     * @param id Profile ID to search for.
     * @return Optional containing DnsProfile if found, std::nullopt otherwise.
     */
    std::optional<DnsProfile> get_profile_by_id(const std::string& id) const;

    /**
     * @brief Returns the ID of the currently active DNS profile.
     */
    std::string get_active_profile_id() const;

    /**
     * @brief Sets the active DNS profile ID in memory (call save_to_file() to persist to disk).
     * @param id The active profile ID.
     */
    void set_active_profile_id(const std::string& id);

    /**
     * @brief Gets the UI theme mode ("dark" or "light"). Defaults to "dark".
     */
    std::string get_theme_mode() const;

    /**
     * @brief Sets the UI theme mode ("dark" or "light").
     */
    void set_theme_mode(const std::string& theme);

    /**
     * @brief Adds a new custom DNS profile to in-memory state (call save_to_file() to persist to disk).
     * Cannot add if ID is empty or already exists.
     * @param profile The profile to add.
     * @return True if added successfully in memory, false otherwise.
     */
    bool add_profile(const DnsProfile& profile);

    /**
     * @brief Updates an existing DNS profile matching the profile ID in memory (call save_to_file() to persist to disk).
     * @param profile The profile containing updated fields.
     * @return True if updated successfully in memory, false if not found.
     */
    bool update_profile(const DnsProfile& profile);

    /**
     * @brief Deletes a custom profile by its ID from in-memory state (call save_to_file() to persist to disk).
     * Built-in profiles cannot be deleted.
     * @param id The ID of the profile to remove.
     * @return True if removed successfully in memory, false if not found or built-in.
     */
    bool delete_profile(const std::string& id);

    /**
     * @brief Saves the current configuration to the JSON file on disk atomically.
     * @return True if successful, false on error.
     */
    bool save_to_file();

    /**
     * @brief Loads the configuration from disk. Populates defaults if file does not exist.
     * @return True if loaded/initialized successfully, false on read or corruption error.
     */
    bool load_from_file();

    /**
     * @brief Returns the configured path to the configuration file.
     */
    std::filesystem::path get_config_file_path() const;

    /**
     * @brief Returns the standard built-in preset DNS profiles.
     */
    static std::vector<DnsProfile> get_default_presets();

private:
    static std::filesystem::path resolve_default_config_path();

    std::filesystem::path m_config_path;
    std::string m_active_profile_id = "dhcp";
    std::string m_theme_mode = "dark";
    std::vector<DnsProfile> m_profiles;
    mutable std::mutex m_mutex;
    mutable std::mutex m_save_mutex;
};
