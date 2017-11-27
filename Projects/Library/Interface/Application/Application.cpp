#include "PrecompiledHeader.hpp"
#include "Interface\LibraryInterface.hpp"
#include "SDR Shared\Json.hpp"
#include "SDR Library API\ExportTypes.hpp"
#include "Application.hpp"
#include "Interface\Application\Extensions\ExtensionManager.hpp"

namespace
{
	struct Application
	{
		std::vector<SDR::ModuleHandlerData> ModuleHandlers;
		std::vector<SDR::StartupFuncData> StartupFunctions;
		std::vector<SDR::ShutdownFuncType> ShutdownFunctions;
	};

	Application MainApplication;

	namespace Memory
	{
		/*
			Not accessing the STL iterators in debug mode makes this run >10x faster, less sitting around waiting for nothing.
		*/
		inline bool DataCompare(const uint8_t* data, const SDR::BytePattern::Entry* pattern, size_t patternlength)
		{
			int index = 0;

			for (size_t i = 0; i < patternlength; i++)
			{
				auto byte = *pattern;

				if (!byte.Unknown && *data != byte.Value)
				{
					return false;
				}
				
				++data;
				++pattern;
				++index;
			}

			return index == patternlength;
		}

		void* FindPattern(void* start, size_t searchlength, const SDR::BytePattern& pattern)
		{
			auto patternstart = pattern.Bytes.data();
			auto length = pattern.Bytes.size();
			
			for (size_t i = 0; i <= searchlength - length; ++i)
			{
				auto addr = (const uint8_t*)(start) + i;
				
				if (DataCompare(addr, patternstart, length))
				{
					return (void*)(addr);
				}
			}

			return nullptr;
		}
	}

	namespace Config
	{
		namespace Registry
		{
			struct DataType
			{
				struct TypeIndex
				{
					enum Type
					{
						Invalid,
						UInt32,
					};
				};

				template <typename T>
				struct TypeReturn
				{
					explicit operator bool() const
					{
						return Address && Type != TypeIndex::Invalid;
					}

					template <typename T>
					void Set(T value)
					{
						if (*this)
						{
							*Address = value;
						}
					}

					T Get() const
					{
						return *Address;
					}

					bool IsUInt32() const
					{
						return Type == TypeIndex::UInt32;
					}

					TypeIndex::Type Type = TypeIndex::Invalid;
					T* Address = nullptr;
				};

				const char* Name = nullptr;
				TypeIndex::Type TypeNumber = TypeIndex::Invalid;

				union
				{
					uint32_t Value_U32;
				};

				template <typename T>
				void SetValue(T value)
				{
					if (std::is_same<T, uint32_t>::value)
					{
						TypeNumber = TypeIndex::Type::UInt32;
						Value_U32 = value;
					}
				}

				template <typename T>
				TypeReturn<T> GetActiveValue()
				{
					TypeReturn<T> ret;
					ret.Type = TypeNumber;

					switch (TypeNumber)
					{
						case TypeIndex::UInt32:
						{
							ret.Address = &Value_U32;							
							return ret;
						}
					}

					return {};
				}
			};

			std::vector<DataType> KeyValues;

			template <typename T>
			void InsertKeyValue(const char* name, T value)
			{
				DataType newtype;
				newtype.Name = name;
				newtype.SetValue(value);

				KeyValues.emplace_back(std::move(newtype));
			}
		}

		template <typename NodeType, typename FuncType>
		void MemberLoop(NodeType& node, FuncType callback)
		{
			auto& begin = node.MemberBegin();
			auto& end = node.MemberEnd();

			for (auto it = begin; it != end; ++it)
			{
				callback(it);
			}
		}

		struct ConfigObjectData
		{
			std::string ObjectName;
			std::vector<std::pair<std::string, rapidjson::Value>> Properties;
		};

		std::vector<ConfigObjectData> GameConfigs;
		std::vector<ConfigObjectData> ExtensionConfigs;

		void ResolveInherit(ConfigObjectData* targetgame, const std::vector<ConfigObjectData>& source, rapidjson::Document::AllocatorType& alloc)
		{
			auto begin = targetgame->Properties.begin();
			auto end = targetgame->Properties.end();

			auto foundinherit = false;

			for (auto it = begin; it != end; ++it)
			{
				if (it->first == "Inherit")
				{
					foundinherit = true;

					if (!it->second.IsString())
					{
						SDR::Error::Make("SDR: \"%s\" inherit field not a string\n", targetgame->ObjectName.c_str());
					}

					std::string from = it->second.GetString();

					targetgame->Properties.erase(it);

					for (const auto& game : source)
					{
						bool foundgame = false;

						if (game.ObjectName == from)
						{
							foundgame = true;

							for (const auto& sourceprop : game.Properties)
							{
								bool shouldadd = true;

								for (const auto& destprop : targetgame->Properties)
								{
									if (sourceprop.first == destprop.first)
									{
										shouldadd = false;
										break;
									}
								}

								if (shouldadd)
								{
									targetgame->Properties.emplace_back(sourceprop.first, rapidjson::Value(sourceprop.second, alloc));
								}
							}

							break;
						}

						if (!foundgame)
						{
							SDR::Error::Make("\"%s\" inherit target \"%s\" not found", targetgame->ObjectName.c_str(), from.c_str());
						}
					}

					break;
				}
			}

			if (foundinherit)
			{
				ResolveInherit(targetgame, source, alloc);
			}
		}

		void PrintModuleState(bool value, const char* name)
		{
			if (!value)
			{
				SDR::Log::Warning("SDR: No handler found for \"%s\"\n", name);
			}

			else
			{
				SDR::Log::Message("SDR: Enabled module \"%s\"\n", name);
			}
		}

		void CallGameHandlers(ConfigObjectData* game)
		{
			SDR::Log::Message("SDR: Creating %d game modules\n", MainApplication.ModuleHandlers.size());

			for (auto& prop : game->Properties)
			{
				/*
					Ignore these, they are only used by the launcher.
				*/
				if (prop.first == "DisplayName")
				{
					continue;
				}

				else if (prop.first == "ExecutableName")
				{
					continue;
				}

				bool found = false;

				for (auto& handler : MainApplication.ModuleHandlers)
				{
					if (prop.first == handler.Name)
					{
						found = true;

						try
						{
							SDR::Error::ScopedContext e1(handler.Name);

							handler.Function(prop.second);
						}

						catch (const SDR::Error::Exception& error)
						{
							SDR::Error::Make("Could not enable module \"%s\"", handler.Name);
							throw;
						}

						break;
					}
				}

				PrintModuleState(found, prop.first.c_str());
			}

			MainApplication.ModuleHandlers.clear();
		}

		void CallExtensionHandlers(ConfigObjectData* object)
		{
			SDR::Log::Message("SDR: Creating %d extension modules\n", object->Properties.size());

			for (auto& prop : object->Properties)
			{
				auto found = SDR::ExtensionManager::Events::CallHandlers(prop.first.c_str(), prop.second);

				PrintModuleState(found, prop.first.c_str());
			}
		}

		ConfigObjectData* PopulateAndFindObject(rapidjson::Document& document, std::vector<ConfigObjectData>& dest)
		{
			MemberLoop(document, [&](rapidjson::Document::MemberIterator gameit)
			{
				dest.emplace_back();
				auto& curobj = dest.back();

				curobj.ObjectName = gameit->name.GetString();

				MemberLoop(gameit->value, [&](rapidjson::Document::MemberIterator gamedata)
				{
					curobj.Properties.emplace_back(gamedata->name.GetString(), std::move(gamedata->value));
				});
			});

			auto gamename = SDR::Library::GetGamePath();

			for (auto& obj : dest)
			{
				if (SDR::String::EndsWith(gamename, obj.ObjectName.c_str()))
				{
					return &obj;
				}
			}

			return nullptr;
		}

		void SetupGame()
		{
			rapidjson::Document document;

			try
			{
				document = SDR::Json::FromFile(SDR::Library::BuildResourcePath("GameConfig.json"));
			}

			catch (SDR::File::ScopedFile::ExceptionType status)
			{
				SDR::Error::Make("Could not find game config"s);
			}

			auto object = PopulateAndFindObject(document, GameConfigs);

			if (!object)
			{
				SDR::Error::Make("Could not find current game in game config"s);
			}

			ResolveInherit(object, GameConfigs, document.GetAllocator());
			CallGameHandlers(object);

			GameConfigs.clear();
		}

		void SetupExtensions()
		{
			rapidjson::Document document;

			try
			{
				document = SDR::Json::FromFile(SDR::Library::BuildResourcePath("ExtensionConfig.json"));
			}

			catch (SDR::File::ScopedFile::ExceptionType status)
			{
				SDR::Error::Make("Could not find extension config"s);
			}

			auto object = PopulateAndFindObject(document, ExtensionConfigs);

			if (!object)
			{
				SDR::Error::Make("Could not find current game in extension config"s);
			}

			ResolveInherit(object, ExtensionConfigs, document.GetAllocator());
			CallExtensionHandlers(object);

			ExtensionConfigs.clear();
		}
	}

	namespace LoadLibraryIntercept
	{
		namespace Common
		{
			template <typename T>
			using TableType = std::initializer_list<std::pair<const T*, std::function<void()>>>;

			template <typename T>
			void CheckTable(const TableType<T>& table, const T* name)
			{
				for (auto& entry : table)
				{
					if (SDR::String::EndsWith(name, entry.first))
					{
						entry.second();
						break;
					}
				}
			}

			void Load(HMODULE module, const char* name)
			{
				TableType<char> table =
				{
					std::make_pair("server.dll", []()
					{
						/*
							This should be changed in the future.
						*/
						SDR::Library::Load();
					})
				};

				CheckTable(table, name);
			}

			void Load(HMODULE module, const wchar_t* name)
			{
				TableType<wchar_t> table =
				{
					
				};

				CheckTable(table, name);
			}
		}

		namespace A
		{
			SDR::HookModule<decltype(LoadLibraryA)*> ThisHook;

			HMODULE WINAPI Override(LPCSTR name)
			{
				auto ret = ThisHook.GetOriginal()(name);

				if (ret)
				{
					Common::Load(ret, name);
				}

				return ret;
			}
		}

		namespace ExA
		{
			SDR::HookModule<decltype(LoadLibraryExA)*> ThisHook;

			HMODULE WINAPI Override(LPCSTR name, HANDLE file, DWORD flags)
			{
				auto ret = ThisHook.GetOriginal()(name, file, flags);

				if (ret)
				{
					Common::Load(ret, name);
				}

				return ret;
			}
		}

		namespace W
		{
			SDR::HookModule<decltype(LoadLibraryW)*> ThisHook;

			HMODULE WINAPI Override(LPCWSTR name)
			{
				auto ret = ThisHook.GetOriginal()(name);

				if (ret)
				{
					Common::Load(ret, name);
				}

				return ret;
			}
		}

		namespace ExW
		{
			SDR::HookModule<decltype(LoadLibraryExW)*> ThisHook;

			HMODULE WINAPI Override(LPCWSTR name, HANDLE file, DWORD flags)
			{
				auto ret = ThisHook.GetOriginal()(name, file, flags);

				if (ret)
				{
					Common::Load(ret, name);
				}

				return ret;
			}
		}

		void Start()
		{
			SDR::CreateHookAPI(L"kernel32.dll", "LoadLibraryA", A::ThisHook, A::Override);
			SDR::CreateHookAPI(L"kernel32.dll", "LoadLibraryExA", ExA::ThisHook, ExA::Override);
			SDR::CreateHookAPI(L"kernel32.dll", "LoadLibraryW", W::ThisHook, W::Override);
			SDR::CreateHookAPI(L"kernel32.dll", "LoadLibraryExW", ExW::ThisHook, ExW::Override);

			auto codes =
			{
				MH_EnableHook(A::ThisHook.TargetFunction),
				MH_EnableHook(ExA::ThisHook.TargetFunction),
				MH_EnableHook(W::ThisHook.TargetFunction),
				MH_EnableHook(ExW::ThisHook.TargetFunction),
			};

			for (auto code : codes)
			{
				if (code != MH_OK)
				{
					SDR::Error::Make("Could not enable library intercepts"s);
				}
			}
		}

		void End()
		{
			MH_DisableHook(A::ThisHook.TargetFunction);
			MH_DisableHook(ExA::ThisHook.TargetFunction);
			MH_DisableHook(W::ThisHook.TargetFunction);
			MH_DisableHook(ExW::ThisHook.TargetFunction);
		}
	}
}

void SDR::PreEngineSetup()
{
	auto res = MH_Initialize();

	if (res != MH_OK)
	{
		SDR::Error::Make("Could not initialize hooks"s);
	}

	LoadLibraryIntercept::Start();
}

void SDR::Setup()
{
	LoadLibraryIntercept::End();

	ExtensionManager::LoadExtensions();
	
	Config::SetupGame();

	if (SDR::ExtensionManager::HasExtensions())
	{
		Config::SetupExtensions();
	}

	for (auto entry : MainApplication.StartupFunctions)
	{
		try
		{
			SDR::Error::ScopedContext e1(entry.Name);
			entry.Function();
		}

		catch (const SDR::Error::Exception& error)
		{
			SDR::Error::Make("Could not pass startup procedure \"%s\"", entry.Name);
			throw;
		}

		SDR::Log::Message("SDR: Passed startup procedure: \"%s\"\n", entry.Name);
	}

	MainApplication.StartupFunctions.clear();

	ExtensionManager::Events::Ready();
}

void SDR::Close()
{
	for (auto func : MainApplication.ShutdownFunctions)
	{
		func();
	}

	MH_Uninitialize();
}

void SDR::AddStartupFunction(const StartupFuncData& data)
{
	MainApplication.StartupFunctions.emplace_back(data);
}

void SDR::AddShutdownFunction(ShutdownFuncType function)
{
	MainApplication.ShutdownFunctions.emplace_back(function);
}

void SDR::AddModuleHandler(const ModuleHandlerData& data)
{
	MainApplication.ModuleHandlers.emplace_back(data);
}

SDR::ModuleInformation::ModuleInformation(const char* name) : Name(name)
{
	SDR::Error::ScopedContext e1("ModuleInformation"s);

	MODULEINFO info;

	SDR::Error::MS::ThrowIfZero
	(
		K32GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(name), &info, sizeof(info)),
		"Could not get module information for \"%s\"", name
	);

	MemoryBase = info.lpBaseOfDll;
	MemorySize = info.SizeOfImage;
}

SDR::BytePattern SDR::GetPatternFromString(const char* input)
{
	BytePattern ret;

	bool shouldbespace = false;

	while (*input)
	{
		if (SDR::String::IsSpace(*input))
		{
			++input;
			shouldbespace = false;
		}

		else if (shouldbespace)
		{
			SDR::Error::Make("Error in string byte pair formatting"s);
		}

		BytePattern::Entry entry;

		if (std::isxdigit(*input))
		{
			entry.Unknown = false;
			entry.Value = std::strtol(input, nullptr, 16);

			input += 2;

			shouldbespace = true;
		}

		else
		{
			entry.Unknown = true;
			input += 2;

			shouldbespace = true;
		}

		ret.Bytes.emplace_back(entry);
	}

	if (ret.Bytes.empty())
	{
		SDR::Error::Make("Empty byte pattern"s);
	}

	return ret;
}

void* SDR::GetAddressFromPattern(const ModuleInformation& library, const BytePattern& pattern)
{
	return Memory::FindPattern(library.MemoryBase, library.MemorySize, pattern);
}

bool SDR::JsonHasPattern(const rapidjson::Value& value)
{
	if (value.HasMember("Pattern"))
	{
		return true;
	}

	return false;
}

bool SDR::JsonHasVirtualIndexOnly(const rapidjson::Value& value)
{
	if (value.HasMember("VTIndex"))
	{
		return true;
	}

	return false;
}

bool SDR::JsonHasVirtualIndexAndNamePtr(const rapidjson::Value& value)
{
	if (JsonHasVirtualIndexOnly(value))
	{
		if (value.HasMember("VTPtrName"))
		{
			return true;
		}
	}

	return false;
}

bool SDR::JsonHasVariant(const rapidjson::Value& value)
{
	if (value.HasMember("Variant"))
	{
		return true;
	}

	return false;
}

void* SDR::GetAddressFromJsonFlex(const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GetAddressFromJsonFlex"s);

	if (JsonHasPattern(value))
	{
		return GetAddressFromJsonPattern(value);
	}

	else if (JsonHasVirtualIndexAndNamePtr(value))
	{
		return GetVirtualAddressFromJson(value);
	}

	return nullptr;
}

void* SDR::GetAddressFromJsonPattern(const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GetAddressFromJsonPattern"s);

	auto module = SDR::Json::GetString(value, "Module");
	auto patternstr = SDR::Json::GetString(value, "Pattern");

	int offset = 0;
	bool isjump = false;

	{
		auto iter = value.FindMember("Offset");

		if (iter != value.MemberEnd())
		{
			offset = iter->value.GetInt();
		}
	}

	if (value.HasMember("IsRelativeJump"))
	{
		isjump = true;
	}

	auto pattern = GetPatternFromString(patternstr);

	AddressFinder address(module, pattern, offset);
	SDR::Error::ThrowIfNull(address.Get());

	if (isjump)
	{
		SDR::Error::ScopedContext e2("Jump"s);

		RelativeJumpFunctionFinder jumper(address.Get());
		SDR::Error::ThrowIfNull(jumper.Get());

		return jumper.Get();
	}

	return address.Get();
}

int SDR::GetVariantFromJson(const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GetVariantFromJson"s);

	if (JsonHasVariant(value))
	{
		return SDR::Json::GetInt(value, "Variant");
	}

	return 0;
}

void SDR::WarnAboutHookVariant(int variant)
{
	SDR::Log::Warning("SDR: No such hook overload in variant %d\n", variant);
}

void SDR::WarnIfVariantOutOfBounds(int variant, int max)
{
	SDR::Error::ScopedContext e1("WarnIfVariantOutOfBounds"s);

	if (variant < 0 || variant >= max)
	{
		SDR::Error::Make("SDR: Variant overload %d for \"%s\" not in bounds (%d max)\n", variant, max);
	}
}

void* SDR::GetVirtualAddressFromIndex(void* ptr, int index)
{
	auto vtable = *((void***)ptr);
	auto address = vtable[index];

	return address;
}

void* SDR::GetVirtualAddressFromJson(void* ptr, const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GetVirtualAddressFromJson"s);

	auto index = GetVirtualIndexFromJson(value);
	return GetVirtualAddressFromIndex(ptr, index);
}

int SDR::GetVirtualIndexFromJson(const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GetVirtualIndexFromJson"s);

	return SDR::Json::GetInt(value, "VTIndex");
}

void* SDR::GetVirtualAddressFromJson(const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GetVirtualAddressFromJson"s);

	auto instance = SDR::Json::GetString(value, "VTPtrName");

	uint32_t ptrnum;
	auto res = ModuleShared::Registry::GetKeyValue(instance, &ptrnum);

	if (!res)
	{
		SDR::Error::Make("Could not find virtual object name \"%s\"", instance);
	}

	auto ptr = (void*)ptrnum;
	SDR::Error::ThrowIfNull(ptr, "Registry value \"%s\" was null", instance);

	return GetVirtualAddressFromJson(ptr, value);
}

void SDR::ModuleShared::Registry::SetKeyValue(const char* name, uint32_t value)
{
	Config::Registry::InsertKeyValue(name, value);
}

bool SDR::ModuleShared::Registry::GetKeyValue(const char* name, uint32_t* value)
{
	*value = 0;

	for (auto& keyvalue : Config::Registry::KeyValues)
	{
		if (strcmp(keyvalue.Name, name) == 0)
		{
			auto active = keyvalue.GetActiveValue<uint32_t>();

			if (active && value)
			{
				*value = active.Get();
			}

			return true;
		}
	}

	return false;
}

void SDR::GenericVariantInit(ModuleShared::Variant::Entry& entry, const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GenericVariantInit"s);

	auto addr = SDR::GetAddressFromJsonFlex(value);
	auto variant = SDR::GetVariantFromJson(value);

	WarnIfVariantOutOfBounds(variant, entry.VariantCount);

	ModuleShared::SetFromAddress(entry, addr, variant);
}

void SDR::CreateHookBare(HookModuleBare& hook, void* override, void* address)
{
	SDR::Error::ScopedContext e1("CreateHookBare"s);

	hook.TargetFunction = address;
	hook.NewFunction = override;

	auto res = MH_CreateHookEx(hook.TargetFunction, hook.NewFunction, &hook.OriginalFunction);

	if (res != MH_OK)
	{
		SDR::Error::Make("Could not create hook (\"%s\")", MH_StatusToString(res));
	}

	res = MH_EnableHook(hook.TargetFunction);

	if (res != MH_OK)
	{
		SDR::Error::Make("Could not enable hook (\"%s\")", MH_StatusToString(res));
	}
}

void SDR::CreateHookBareShort(HookModuleBare& hook, void* override, const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("CreateHookBareShort"s);

	auto address = GetAddressFromJsonPattern(value);
	CreateHookBare(hook, override, address);
}

void SDR::CreateHookAPI(const wchar_t* module, const char* name, HookModuleBare& hook, void* override)
{
	SDR::Error::ScopedContext e1("CreateHookAPI"s);

	hook.NewFunction = override;
	
	auto res = MH_CreateHookApiEx(module, name, override, &hook.OriginalFunction, &hook.TargetFunction);

	if (res != MH_OK)
	{
		SDR::Error::Make("Could not create API hook \"%s\" (\"%s\")", name, MH_StatusToString(res));
	}
}

void SDR::GenericHookVariantInit(std::initializer_list<GenericHookInitParam> hooks, const rapidjson::Value& value)
{
	SDR::Error::ScopedContext e1("GenericHookVariantInit"s);

	auto variant = SDR::GetVariantFromJson(value);
	auto size = hooks.size();

	WarnIfVariantOutOfBounds(variant, size);

	auto target = *(hooks.begin() + variant);

	CreateHookBareShort(target.Hook, target.Override, value);
}
