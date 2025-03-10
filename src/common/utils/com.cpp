#include "com.hpp"
#include "nt.hpp"

#include <stdexcept>

#include <ShlObj.h>
#include <gsl/gsl>


namespace utils::com
{
	namespace
	{
		[[maybe_unused]] class _
		{
		public:
			_()
			{
				if (FAILED(CoInitialize(nullptr)))
				{
					throw std::runtime_error("Failed to initialize the component object model");
				}
			}

			~_()
			{
				CoUninitialize();
			}
		} __;
	}

	bool select_folder(std::wstring& out_folder, const std::wstring& title, const std::wstring& selected_folder)
	{
		CComPtr<IFileOpenDialog> file_dialog{};
		if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&file_dialog))))
		{
			throw std::runtime_error("Failed to create co instance");
		}

		DWORD dw_options;
		if (FAILED(file_dialog->GetOptions(&dw_options)))
		{
			throw std::runtime_error("Failed to get options");
		}

		if (FAILED(file_dialog->SetOptions(dw_options | FOS_PICKFOLDERS)))
		{
			throw std::runtime_error("Failed to set options");
		}

		if (FAILED(file_dialog->SetTitle(title.data())))
		{
			throw std::runtime_error("Failed to set title");
		}

		if (!selected_folder.empty())
		{
			file_dialog->ClearClientData();

			std::wstring wide_selected_folder = selected_folder;
			for (auto& chr : wide_selected_folder)
			{
				if (chr == L'/')
				{
					chr = L'\\';
				}
			}

			IShellItem* shell_item = nullptr;
			if (FAILED(SHCreateItemFromParsingName(wide_selected_folder.data(), NULL, IID_PPV_ARGS(&shell_item))))
			{
				throw std::runtime_error("Failed to create item from parsing name");
			}

			if (FAILED(file_dialog->SetDefaultFolder(shell_item)))
			{
				throw std::runtime_error("Failed to set default folder");
			}
		}

		const auto result = file_dialog->Show(nullptr);
		if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
		{
			return false;
		}

		if (FAILED(result))
		{
			throw std::runtime_error("Failed to show dialog");
		}

		CComPtr<IShellItem> result_item{};
		if (FAILED(file_dialog->GetResult(&result_item)))
		{
			throw std::runtime_error("Failed to get result");
		}

		PWSTR raw_path = nullptr;
		if (FAILED(result_item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)))
		{
			throw std::runtime_error("Failed to get path display name");
		}

		const auto _ = gsl::finally([raw_path]()
		{
			CoTaskMemFree(raw_path);
		});

		out_folder = raw_path;

		return true;
	}

	CComPtr<IProgressDialog> create_progress_dialog()
	{
		CComPtr<IProgressDialog> progress_dialog{};
		if (FAILED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&progress_dialog))))
		{
			throw std::runtime_error("Failed to create co instance");
		}

		return progress_dialog;
	}
}
