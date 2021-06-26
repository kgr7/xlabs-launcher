#include "std_include.hpp"

#include "cef/cef_ui.hpp"
#include "cef/cef_ui_app.hpp"
#include "cef/cef_ui_handler.hpp"
#include "cef/cef_ui_scheme_handler.hpp"

#include <utils/nt.hpp>
#include <utils/string.hpp>

#define CEF_PATH "cef/" CONFIG_NAME

namespace cef
{
	namespace
	{
		void delay_load_cef(const std::string& path)
		{
			static std::atomic<bool> initialized{false};
			bool uninitialized = false;
			if (!initialized.compare_exchange_strong(uninitialized, true))
			{
				return;
			}

			const auto old_directory = utils::nt::library::get_dll_directory();
			utils::nt::library::set_dll_directory(path);
			auto _ = gsl::finally([&]()
			{
				utils::nt::library::set_dll_directory(old_directory);
			});

			if (!utils::nt::library::load("libcef.dll"s) //
				|| !utils::nt::library::delay_load("libcef.dll"))
			{
				throw std::runtime_error("Failed to load CEF");
			}
		}

		void scale_dpi(CefWindowInfo& info)
		{
			const utils::nt::library user32{"user32.dll"};
			const auto get_dpi = user32 ? user32.get_proc<UINT(WINAPI *)(HWND)>("GetDpiForWindow") : nullptr;
			const auto unaware_dpi = 96;

			if (get_dpi)
			{
				const auto dpi = get_dpi(GetForegroundWindow());

				info.width *= dpi;
				info.width /= unaware_dpi;

				info.height *= dpi;
				info.height /= unaware_dpi;
			}
		}
	}

	void cef_ui::work_once()
	{
		CefDoMessageLoopWork();
	}

	void cef_ui::work()
	{
		CefRunMessageLoop();
	}

	int cef_ui::run_process() const
	{
		const CefMainArgs args(this->process_.get_handle());
		return CefExecuteProcess(args, nullptr, nullptr);
	}

	void cef_ui::create(const std::string& folder, const std::string& file)
	{
		if (this->browser_) return;

		CefMainArgs args(this->process_.get_handle());

		CefSettings settings;
		settings.no_sandbox = TRUE;
		//settings.single_process = TRUE;
		//settings.windowless_rendering_enabled = TRUE;
		//settings.pack_loading_disabled = FALSE;
		settings.remote_debugging_port = 12345;

#ifdef DEBUG
		settings.log_severity = LOGSEVERITY_VERBOSE;
#else
		settings.log_severity = LOGSEVERITY_DISABLE;
#endif

		CefString(&settings.browser_subprocess_path) = this->process_.get_path();
		CefString(&settings.locales_dir_path) = this->path_ + (CEF_PATH "/locales");
		CefString(&settings.resources_dir_path) = this->path_ + CEF_PATH;
		CefString(&settings.log_file) = this->path_ + "cef-data/debug.log";
		CefString(&settings.user_data_path) = this->path_ + "cef-data/user";
		CefString(&settings.cache_path) = this->path_ + "cef-data/cache";
		CefString(&settings.locale) = "en-US";

		this->initialized_ = CefInitialize(args, settings, new cef_ui_app(), nullptr);
		CefRegisterSchemeHandlerFactory("http", "xlabs", new cef_ui_scheme_handler_factory(folder));

		CefBrowserSettings browser_settings;
		//browser_settings.windowless_frame_rate = 60;

		CefWindowInfo window_info;
		window_info.SetAsPopup(nullptr, "X Labs");
		window_info.width = 800; //GetSystemMetrics(SM_CXVIRTUALSCREEN);
		window_info.height = 500; //GetSystemMetrics(SM_CYVIRTUALSCREEN);
		window_info.x = (GetSystemMetrics(SM_CXSCREEN) - window_info.width) / 2;
		window_info.y = (GetSystemMetrics(SM_CYSCREEN) - window_info.height) / 2;
		window_info.style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VISIBLE);

		scale_dpi(window_info);

		if (!this->ui_handler_)
		{
			this->ui_handler_ = new cef_ui_handler();
		}

		const auto url = "http://xlabs/" + file;
		this->browser_ = CefBrowserHost::CreateBrowserSync(window_info, this->ui_handler_, url, browser_settings,
		                                                   nullptr, nullptr);

		this->set_window_icon();

		auto window = this->get_window();
		std::thread([window]()
		{
			std::this_thread::sleep_for(1000ms);
			ShowWindow(window, SW_SHOWDEFAULT);
		}).detach();
	}

	void cef_ui::set_window_icon() const
	{
		auto* const window = this->get_window();
		if (!window) return;

		const auto icon = LPARAM(LoadIconA(this->process_.get_handle(), MAKEINTRESOURCEA(IDI_ICON_1)));
		SendMessageA(window, WM_SETICON, ICON_SMALL, icon);
		SendMessageA(window, WM_SETICON, ICON_BIG, icon);
	}

	HWND cef_ui::get_window() const
	{
		if (!this->browser_) return nullptr;
		return this->browser_->GetHost()->GetWindowHandle();
	}

	void cef_ui::invoke_close_browser(CefRefPtr<CefBrowser> browser)
	{
		if (!browser) return;
		browser->GetHost()->CloseBrowser(true);
	}

	void cef_ui::close_browser()
	{
		if (!this->browser_) return;
		CefPostTask(TID_UI, base::Bind(&cef_ui::invoke_close_browser, this->browser_));
		this->browser_ = nullptr;
	}

	void cef_ui::reload_browser() const
	{
		if (!this->browser_) return;
		this->browser_->Reload();
	}

	cef_ui::cef_ui(utils::nt::library process, std::string path)
		: process_(std::move(process)), path_(std::move(path))
	{
		delay_load_cef(this->path_ + CEF_PATH);
		CefEnableHighDPISupport();
	}

	cef_ui::~cef_ui()
	{
		if (this->browser_ //
			&& this->ui_handler_ //
			&& !this->ui_handler_->is_closed(this->browser_))
		{
			this->close_browser();
			this->work();
		}

		this->browser_ = {};
		this->ui_handler_ = {};

		if (this->initialized_)
		{
			CefShutdown();
		}
	}
}