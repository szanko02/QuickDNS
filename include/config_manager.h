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
    bool is_builtin = false;
    bool is_dhcp = false;

    bool operator==(const DnsProfile& other) const = default;
};

/**
 * @brief Manages loading, saving, updating and querying DNS configuration profiles.
 */
class ConfigManager {
public:
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
     * @brief Sets the active DNS profile ID and saves the configuration.
     * @param id The active profile ID.
     */
    void set_active_profile_id(const std::string& id);

    /**
     * @brief Adds a new DNS profile. Cannot add if ID is empty or already exists.
     * @param profile The profile to add.
     * @return True if added and saved successfully, false otherwise.
     */
    bool add_profile(const DnsProfile& profile);

    /**
     * @brief Updates an existing DNS profile matching the profile ID.
     * @param profile The profile containing updated fields.
     * @return True if updated and saved successfully, false if not found.
     */
    bool update_profile(const DnsProfile& profile);

    /**
     * @brief Deletes a profile by its ID. Built-in profiles cannot be deleted.
     * @param id The ID of the profile to remove.
     * @return True if removed and saved successfully, false if not found or built-in.
     */
    bool delete_profile(const std::string& id);

    /**
     * @brief Saves the current configuration to the JSON file on disk.
     * @return True if successful, false on error.
     */
    bool save_to_file();

    /**
     * @brief Loads the configuration from disk. Populates defaults if file does not exist.
     * @return True if loaded/initialized successfully, false on unrecoverable error.
     */
    bool load_from_file();

    /**
     * @brief Returns the absolute path to the configuration file.
     */
    std::filesystem::path get_config_file_path() const;

    /**
     * @brief Returns the standard built-in preset DNS profiles.
     */
    static std::vector<DnsProfile> get_default_presets();

private:
    static std::filesystem::path resolve_default_config_path();
    bool save_to_file_internal();

    std::filesystem::path m_config_path;
    std::string m_active_profile_id = "dhcp";
    std::vector<DnsProfile> m_profiles;
    mutable std::mutex m_mutex;
};
