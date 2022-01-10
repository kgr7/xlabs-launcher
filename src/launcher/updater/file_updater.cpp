#include "std_include.hpp"

#include "updater.hpp"
#include "updater_ui.hpp"
#include "file_updater.hpp"

#include <utils/cryptography.hpp>
#include <utils/http.hpp>
#include <utils/io.hpp>

#include <rapidjson/document.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/writer.h>

#define UPDATE_SERVER "https://master.xlabs.dev/"

#define UPDATE_FILE_MAIN UPDATE_SERVER "files.json"
#define UPDATE_FOLDER_MAIN UPDATE_SERVER "data/"

#define UPDATE_FILE_DEV UPDATE_SERVER "files-dev.json"
#define UPDATE_FOLDER_DEV UPDATE_SERVER "data-dev/"

#define UPDATE_HOST_BINARY "xlabs.exe"

#define IW4X_VERSION_FILE ".version.json";

namespace updater
{
	namespace
	{
		std::string get_update_file()
		{
			return is_main_channel() ? UPDATE_FILE_MAIN : UPDATE_FILE_DEV;
		}

		std::string get_update_folder()
		{
			return is_main_channel() ? UPDATE_FOLDER_MAIN : UPDATE_FOLDER_DEV;
		}

		std::vector<file_info> parse_file_infos(const std::string& json)
		{
			rapidjson::Document doc{};
			doc.Parse(json.data(), json.size());

			if (!doc.IsArray())
			{
				return {};
			}

			std::vector<file_info> files{};

			for (const auto& element : doc.GetArray())
			{
				if (!element.IsArray())
				{
					continue;
				}

				auto array = element.GetArray();

				file_info info{};
				info.name.assign(array[0].GetString(), array[0].GetStringLength());
				info.size = array[1].GetInt64();
				info.hash.assign(array[2].GetString(), array[2].GetStringLength());

				files.emplace_back(std::move(info));
			}

			return files;
		}

		std::vector<file_info> get_file_infos()
		{
			const auto json = utils::http::get_data(get_update_file());
			if (!json)
			{
				return {};
			}

			return parse_file_infos(*json);
		}

		std::string get_hash(const std::string& data)
		{
			return utils::cryptography::sha1::compute(data, true);
		}

		const file_info* find_host_file_info(const std::vector<file_info>& outdated_files)
		{
			for (const auto& file : outdated_files)
			{
				if (file.name == UPDATE_HOST_BINARY)
				{
					return &file;
				}
			}

			return nullptr;
		}

		size_t get_optimal_concurrent_download_count(const size_t file_count)
		{
			size_t cores = std::thread::hardware_concurrency();
			cores = (cores * 2) / 3;
			return std::max(1ull, std::min(cores, file_count));
		}

		bool is_inside_folder(const std::filesystem::path& file, const std::filesystem::path& folder)
		{
			const auto relative = std::filesystem::relative(file, folder);
			const auto start = relative.begin();
			return start != relative.end() && start->string() != "..";
		}
	}

	file_updater::file_updater(progress_listener& listener, std::string base, std::string process_file)
		: listener_(listener)
		  , base_(std::move(base))
		  , process_file_(std::move(process_file))
	{
		this->dead_process_file_ = this->process_file_ + ".old";
		this->delete_old_process_file();
	}

	void file_updater::run() const
	{
		const auto files = get_file_infos();
		if (!files.empty())
		{
			this->cleanup_directories(files);
		}

		const auto outdated_files = this->get_outdated_files(files);
		if (outdated_files.empty())
		{
			return;
		}

		this->update_host_binary(outdated_files);
		this->update_files(outdated_files);
	}

	void file_updater::update_file(const file_info& file) const
	{
		const auto url = get_update_folder() + file.name;
		const auto data = utils::http::get_data(url, {}, [&](const size_t progress)
		{
			this->listener_.file_progress(file, progress);
		});

		if (!data || data->size() != file.size || get_hash(*data) != file.hash)
		{
			throw std::runtime_error("Failed to download: " + url);
		}

		const auto out_file = this->get_drive_filename(file);
		if (!utils::io::write_file(out_file, *data, false))
		{
			throw std::runtime_error("Failed to write: " + file.name);
		}
	}

	std::vector<file_info> file_updater::get_outdated_files(const std::vector<file_info>& files) const
	{
		std::vector<file_info> outdated_files{};

		for (const auto& info : files)
		{
			if (this->is_outdated_file(info))
			{
				outdated_files.emplace_back(info);
			}
		}

		return outdated_files;
	}

	void file_updater::update_host_binary(const std::vector<file_info>& outdated_files) const
	{
		const auto* host_file = find_host_file_info(outdated_files);
		if (!host_file)
		{
			return;
		}

		try
		{
			this->move_current_process_file();
			this->update_files({*host_file});
		}
		catch (...)
		{
			this->restore_current_process_file();
			throw;
		}

		utils::nt::relaunch_self();
		throw update_cancelled();
	}

	bool file_updater::does_iw4x_require_update(std::filesystem::path iw4x_basegame_directory, bool& out_requires_rawfile_update, bool& out_requires_iw4x_update) const
	{
		std::filesystem::path revision_file_path = iw4x_basegame_directory / IW4X_VERSION_FILE;

		out_requires_rawfile_update = true;
		out_requires_iw4x_update = true;

		rapidjson::Document doc{};
		doc.SetObject();

		std::string data{};
		const auto& props = revision_file_path.string();
		if (!utils::io::read_file(props, &data))
		{
			return true;
		}

		rapidjson::Document doc{};
		const rapidjson::ParseResult result = doc.Parse(data);
		if (!result || !doc.IsObject())
		{
			return true;
		}

		if (doc.HasMember("iw4x_version"))
		{
			std::optional<std::string> iw4x_tag = get_release_tag("https://api.github.com/repos/XLabsProject/iw4x-client/releases/latest");
			if (iw4x_tag.has_value())
			{
				out_requires_iw4x_update = doc["iw4x_version"].GetString() != iw4x_tag;
			}
		}

		if (doc.HasMember("rawfile_version"))
		{
			std::optional<std::string> rawfiles_tag = get_release_tag("https://api.github.com/repos/XLabsProject/iw4x-rawfiles/releases/latest");
			if (rawfiles_tag.has_value())
			{
				out_requires_rawfile_update = doc["rawfile_version"].GetString() != rawfiles_tag;
			}
		}

		return out_requires_iw4x_update || out_requires_rawfile_update;
	}

	std::optional<std::string> file_updater::get_release_tag(std::string release_url) const
	{
		std::optional<std::string> iw4x_release_info = utils::http::get_data(release_url);
		if (iw4x_release_info.has_value())
		{
			rapidjson::Document release_json{};
			release_json.Parse(iw4x_release_info.value());

			if (release_json.HasMember("tag_name"))
			{
				auto tag_name = release_json["tag_name"].GetString();

				return release_json["tag_name"].GetString();
			}
		}

		return std::optional<std::string>();
	}

	void file_updater::create_iw4x_version_file(std::filesystem::path iw4x_basegame_directory, std::string rawfile_version, std::string iw4x_version) const
	{
		rapidjson::Document doc{};

		rapidjson::StringBuffer buffer{};
		rapidjson::Writer<rapidjson::StringBuffer, rapidjson::Document::EncodingType, rapidjson::ASCII<>>
			writer(buffer);

		doc.Accept(writer);

		doc.AddMember("rawfile_version", rawfile_version, doc.GetAllocator());
		doc.AddMember("iw4x_version", iw4x_version, doc.GetAllocator());

		const std::string json(buffer.GetString(), buffer.GetLength());

		std::filesystem::path revision_file_path = iw4x_basegame_directory / IW4X_VERSION_FILE;

		utils::io::write_file(revision_file_path.string(), json);
	}

	void file_updater::update_iw4x_if_necessary(std::filesystem::path iw4x_basegame_directory) const
	{
		bool requires_rawfile_update, requires_iw4x_update; 

		if (does_iw4x_require_update(iw4x_basegame_directory , requires_rawfile_update, requires_iw4x_update))
		{
			std::vector<file_info> files_to_update{};

			if (requires_rawfile_update)
			{
				files_to_update.emplace_back("https://github.com/XLabsProject/iw4x-client/releases/latest/download/iw4x.dll");
			}

			if (requires_iw4x_update)
			{
				files_to_update.emplace_back("https://github.com/XLabsProject/iw4x-rawfiles/releases/latest/download/release.zip");
			}

			update_files(files_to_update);
			create_iw4x_version_file(iw4x_basegame_directory, "TEST", "TEST 2");

			// #TODO: 
			// Once this is done, we need to move iw4x.dll over and to unpack release.zip and prepare rawfiles
		}
	}

	void file_updater::update_files(const std::vector<file_info>& outdated_files) const
	{
		this->listener_.update_files(outdated_files);

		const auto thread_count = get_optimal_concurrent_download_count(outdated_files.size());

		std::vector<std::thread> threads{};
		std::atomic<size_t> current_index{0};


		utils::concurrency::container<std::exception_ptr> exception{};

		for (size_t i = 0; i < thread_count; ++i)
		{
			threads.emplace_back([&]()
			{
				while (!exception.access<bool>([](const std::exception_ptr& ptr)
				{
					return static_cast<bool>(ptr);
				}))
				{
					const auto index = current_index++;
					if (index >= outdated_files.size())
					{
						break;
					}

					try
					{
						const auto& file = outdated_files[index];
						this->listener_.begin_file(file);
						this->update_file(file);
						this->listener_.end_file(file);
					}
					catch (...)
					{
						exception.access([](std::exception_ptr& ptr)
						{
							ptr = std::current_exception();
						});

						return;
					}
				}
			});
		}

		for (auto& thread : threads)
		{
			if (thread.joinable())
			{
				thread.join();
			}
		}

		exception.access([](const std::exception_ptr& ptr)
		{
			if (ptr)
			{
				std::rethrow_exception(ptr);
			}
		});

		this->listener_.done_update();
	}

	bool file_updater::is_outdated_file(const file_info& file) const
	{
#ifndef CI_BUILD
		if (file.name == UPDATE_HOST_BINARY)
		{
			return false;
		}
#endif

		std::string data{};
		const auto drive_name = this->get_drive_filename(file);
		if (!utils::io::read_file(drive_name, &data))
		{
			return true;
		}

		if (data.size() != file.size)
		{
			return true;
		}

		const auto hash = get_hash(data);
		return hash != file.hash;
	}

	std::string file_updater::get_drive_filename(const file_info& file) const
	{
		if (file.name == UPDATE_HOST_BINARY)
		{
			return this->process_file_;
		}

		return this->base_ + "data/" + file.name;
	}

	void file_updater::move_current_process_file() const
	{
		utils::io::move_file(this->process_file_, this->dead_process_file_);
	}

	void file_updater::restore_current_process_file() const
	{
		utils::io::move_file(this->dead_process_file_, this->process_file_);
	}

	void file_updater::delete_old_process_file() const
	{
		// Wait for other process to die
		for (auto i = 0; i < 4; ++i)
		{
			utils::io::remove_file(this->dead_process_file_);
			if (!utils::io::file_exists(this->dead_process_file_))
			{
				break;
			}

			std::this_thread::sleep_for(2s);
		}
	}

	void file_updater::cleanup_directories(const std::vector<file_info>& files) const
	{
		if (!utils::io::directory_exists(this->base_))
		{
			return;
		}

		this->cleanup_root_directory();
		this->cleanup_data_directory(files);
	}

	void file_updater::cleanup_root_directory() const
	{
		const auto existing_files = utils::io::list_files(this->base_);
		for (const auto& file : existing_files)
		{
			const auto entry = std::filesystem::relative(file, this->base_);
			if ((entry.string() == "user" || entry.string() == "data") && utils::io::directory_exists(file))
			{
				continue;
			}

			std::error_code code{};
			std::filesystem::remove_all(file, code);
		}
	}

	void file_updater::cleanup_data_directory(const std::vector<file_info>& files) const
	{
		const auto base = std::filesystem::path(this->base_) / "data";
		if (!utils::io::directory_exists(base.string()))
		{
			return;
		}

		std::vector<std::filesystem::path> legal_files{};
		legal_files.reserve(files.size());
		for (const auto& file : files)
		{
			if (file.name != UPDATE_HOST_BINARY)
			{
				legal_files.emplace_back(std::filesystem::absolute(base / file.name));
			}
		}

		const auto existing_files = utils::io::list_files(base.string(), true);
		for (auto& file : existing_files)
		{
			const auto is_file = std::filesystem::is_regular_file(file);
			const auto is_folder = std::filesystem::is_directory(file);

			if (is_file || is_folder)
			{
				bool is_legal = false;

				for (const auto& legal_file : legal_files)
				{
					if ((is_folder && is_inside_folder(legal_file, file)) ||
						(is_file && legal_file == file))
					{
						is_legal = true;
						break;
					}
				}

				if (is_legal)
				{
					continue;
				}
			}

			std::error_code code{};
			std::filesystem::remove_all(file, code);
		}
	}
}
