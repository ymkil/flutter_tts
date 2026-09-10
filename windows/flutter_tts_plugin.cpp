#include "include/flutter_tts/flutter_tts_plugin.h"
// This must be included before many other Windows headers.
#include <windows.h>
#include <ppltasks.h>
#include <VersionHelpers.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <map>
#include <memory>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <winrt/Windows.Foundation.h>

typedef std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> FlutterResult;
//typedef flutter::MethodResult<flutter::EncodableValue>* PFlutterResult;

std::unique_ptr<flutter::MethodChannel<>> methodChannel;

namespace {
	struct SynthesisState {
		bool active = false;
		bool alive = true;
	};

	// 任务仅持有独立状态，插件销毁后不再访问实例或发送回调。
	winrt::Windows::Foundation::IAsyncAction writeSynthesis(
		std::function<void()> writeFile, std::shared_ptr<SynthesisState> state,
		FlutterResult result, winrt::apartment_context caller) {
		std::string error;
		co_await winrt::resume_background();
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			struct ApartmentGuard { ~ApartmentGuard() { winrt::uninit_apartment(); } } apartment;
			writeFile();
		}
		catch (const winrt::hresult_error& e) {
			error = winrt::to_string(e.message());
			if (error.empty()) error = "Speech synthesis failed";
		}
		catch (const std::exception& e) {
			error = e.what();
		}
		catch (...) {
			error = "Speech synthesis failed";
		}
		// MethodChannel 和 MethodResult 必须在调用线程上使用。
		try { co_await caller; }
		catch (...) { co_return; }
		state->active = false;
		if (!state->alive) co_return;
		if (error.empty()) {
			methodChannel->InvokeMethod("synth.onComplete", nullptr);
			if (result) result->Success(1);
		}
		else {
			methodChannel->InvokeMethod("synth.onError",
				std::make_unique<flutter::EncodableValue>(error));
			if (result) result->Success(0);
		}
	}
}

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_DESKTOP_APP)
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Storage.Streams.h>
using namespace winrt;
using namespace Windows::Media::SpeechSynthesis;
using namespace Concurrency;
using namespace std::chrono_literals;
#include <winrt/Windows.Foundation.Collections.h>
namespace {
	class FlutterTtsPlugin : public flutter::Plugin {
	public:
		static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);
		FlutterTtsPlugin();
		virtual ~FlutterTtsPlugin();
	private:
		// Called when a method is called on this plugin's channel from Dart.
		void HandleMethodCall(
			const flutter::MethodCall<flutter::EncodableValue>& method_call,
			std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
		void speak(const std::string, FlutterResult);
		std::function<void()> synthesisWriter(const std::string&, const std::filesystem::path&);
		bool awaitSynthCompletion = false;
		std::shared_ptr<SynthesisState> synthesisState = std::make_shared<SynthesisState>();
		void pause();
		void continuePlay();
		void stop();
		void setVolume(const double);
		void setPitch(const double);
		void setRate(const double);
		void getVoices(flutter::EncodableList&);
		void setVoice(const std::string, const std::string, FlutterResult&);
		void getLanguages(flutter::EncodableList&);
		void setLanguage(const std::string, FlutterResult&);
		void addMplayer();
		winrt::Windows::Foundation::IAsyncAction asyncSpeak(const std::string);
		bool speaking();
		bool paused();
		SpeechSynthesizer synth;
		winrt::Windows::Media::Playback::MediaPlayer mPlayer;
		bool isPaused;
		bool isSpeaking;
		bool awaitSpeakCompletion;
		FlutterResult speakResult;
	};

	void FlutterTtsPlugin::RegisterWithRegistrar(
		flutter::PluginRegistrarWindows* registrar) {
		methodChannel =
			std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
				registrar->messenger(), "flutter_tts",
				&flutter::StandardMethodCodec::GetInstance());
		auto plugin = std::make_unique<FlutterTtsPlugin>();

		methodChannel->SetMethodCallHandler(
			[plugin_pointer = plugin.get()](const auto& call, auto result) {
			plugin_pointer->HandleMethodCall(call, std::move(result));
		});
		registrar->AddPlugin(std::move(plugin));
	}

	void FlutterTtsPlugin::addMplayer() {
		mPlayer = winrt::Windows::Media::Playback::MediaPlayer::MediaPlayer();
		auto mEndedToken =
			mPlayer.MediaEnded([=](Windows::Media::Playback::MediaPlayer const& sender,
				Windows::Foundation::IInspectable const& args)
				{
				    methodChannel->InvokeMethod("speak.onComplete", NULL);
				    if (awaitSpeakCompletion) {
                        speakResult->Success(1);
                    }
					isSpeaking = false;
				});
	}

	bool FlutterTtsPlugin::speaking() {
		return isSpeaking;
	}

	bool FlutterTtsPlugin::paused() {
		return isPaused;
	}

	winrt::Windows::Foundation::IAsyncAction FlutterTtsPlugin::asyncSpeak(const std::string text) {
		SpeechSynthesisStream speechStream{
		  co_await synth.SynthesizeTextToStreamAsync(to_hstring(text))
		};
		winrt::param::hstring cType = L"Audio";
		winrt::Windows::Media::Core::MediaSource source =
			winrt::Windows::Media::Core::MediaSource::CreateFromStream(speechStream, cType);
		mPlayer.Source(source);
		mPlayer.Play();
	}

	void FlutterTtsPlugin::speak(const std::string text, FlutterResult result) {
		isSpeaking = true;
		auto my_task{ asyncSpeak(text) };
		methodChannel->InvokeMethod("speak.onStart", NULL);
        if (awaitSpeakCompletion) speakResult = std::move(result);
        else result->Success(1);
	};

	std::function<void()> FlutterTtsPlugin::synthesisWriter(
		const std::string& text, const std::filesystem::path& path) {
		const auto voiceId = synth.Voice().Id();
		const auto volume = synth.Options().AudioVolume();
		const auto pitchValue = synth.Options().AudioPitch();
		const auto rate = synth.Options().SpeakingRate();
		return [text, path, voiceId, volume, pitchValue, rate]() {
			// 独立合成器保留调用时的设置，不改变正在播放的语音。
			SpeechSynthesizer fileSynth;
			for (const auto& voice : SpeechSynthesizer::AllVoices()) {
				if (voice.Id() == voiceId) { fileSynth.Voice(voice); break; }
			}
			fileSynth.Options().AudioVolume(volume);
			fileSynth.Options().AudioPitch(pitchValue);
			fileSynth.Options().SpeakingRate(rate);
			auto stream = fileSynth.SynthesizeTextToStreamAsync(to_hstring(text)).get();
			Windows::Storage::Streams::DataReader reader(stream);
			std::ofstream output;
			output.exceptions(std::ios::failbit | std::ios::badbit);
			output.open(path, std::ios::binary | std::ios::trunc);
			// 分块写入，避免长文本对应的音频一次性占用大量内存。
			std::vector<uint8_t> buffer(65536);
			uint64_t remaining = stream.Size();
			while (remaining > 0) {
				const auto count = static_cast<uint32_t>((std::min)(remaining, uint64_t(buffer.size())));
				if (reader.LoadAsync(count).get() != count) {
					throw std::runtime_error("Incomplete speech synthesis stream");
				}
				reader.ReadBytes(winrt::array_view<uint8_t>(buffer.data(), buffer.data() + count));
				output.write(reinterpret_cast<const char*>(buffer.data()), count);
				remaining -= count;
			}
			output.close();
			reader.Close();
			stream.Close();
			fileSynth.Close();
		};
	}

	void FlutterTtsPlugin::pause() {
		mPlayer.Pause();
		isPaused = true;
		methodChannel->InvokeMethod("speak.onPause", NULL);
	}

	void FlutterTtsPlugin::continuePlay() {
		mPlayer.Play();
		isPaused = false;
		methodChannel->InvokeMethod("speak.onContinue", NULL);
	}

	void FlutterTtsPlugin::stop() {
	    methodChannel->InvokeMethod("speak.onCancel", NULL);
        if (awaitSpeakCompletion) {
            speakResult->Success(1);
        }

		mPlayer.Close();
		addMplayer();
		isSpeaking = false;
		isPaused = false;
	}
	void FlutterTtsPlugin::setVolume(const double newVolume) { synth.Options().AudioVolume(newVolume); }

	void FlutterTtsPlugin::setPitch(const double newPitch) { synth.Options().AudioPitch(newPitch); }

	void FlutterTtsPlugin::setRate(const double newRate) { synth.Options().SpeakingRate(newRate + 0.5); }

	void FlutterTtsPlugin::getVoices(flutter::EncodableList& voices) {
		auto synthVoices = synth.AllVoices();
		std::for_each(begin(synthVoices), end(synthVoices), [&voices](const VoiceInformation& voice)
			{
				flutter::EncodableMap voiceInfo;
				voiceInfo[flutter::EncodableValue("locale")] = to_string(voice.Language());
				voiceInfo[flutter::EncodableValue("name")] = to_string(voice.DisplayName());
				//  Convert VoiceGender to string
				std::string gender;
				switch (voice.Gender()) {
					case VoiceGender::Male:
						gender = "male";
						break;
					case VoiceGender::Female:
						gender = "female";
						break;
					default:
						gender = "unknown";
						break;
				}
				voiceInfo[flutter::EncodableValue("gender")] = gender; 
				// Identifier example "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech_OneCore\Voices\Tokens\MSTTS_V110_enUS_MarkM"
				voiceInfo[flutter::EncodableValue("identifier")] = to_string(voice.Id());
				voices.push_back(flutter::EncodableMap(voiceInfo));
			});
	}

	void FlutterTtsPlugin::setVoice(const std::string voiceLanguage, const std::string voiceName, FlutterResult& result) {
		bool found = false;
		auto voices = synth.AllVoices();
		VoiceInformation newVoice = synth.Voice();
		std::for_each(begin(voices), end(voices), [&voiceLanguage, &voiceName, &found, &newVoice](const VoiceInformation& voice)
			{
				if (to_string(voice.Language()) == voiceLanguage && to_string(voice.DisplayName()) == voiceName)
				{
					newVoice = voice;
					found = true;
				}
			});
		synth.Voice(newVoice);
		if (found) result->Success(1);
		else result->Success(0);
	}

	void FlutterTtsPlugin::getLanguages(flutter::EncodableList& languages) {
		auto synthVoices = synth.AllVoices();
		std::set<flutter::EncodableValue> languagesSet = {};
		std::for_each(begin(synthVoices), end(synthVoices), [&languagesSet](const VoiceInformation& voice)
			{
				languagesSet.insert(flutter::EncodableValue(to_string(voice.Language())));
			});
		std::for_each(begin(languagesSet), end(languagesSet), [&languages](const flutter::EncodableValue value)
			{
				languages.push_back(value);
			});
	}
	void FlutterTtsPlugin::setLanguage(const std::string voiceLanguage, FlutterResult& result) {
		bool found = false;
		auto voices = synth.AllVoices();
		VoiceInformation newVoice = synth.Voice();
		std::for_each(begin(voices), end(voices), [&voiceLanguage, &newVoice, &found](const VoiceInformation& voice)
			{
				if (to_string(voice.Language()) == voiceLanguage) newVoice = voice;
				found = true;
			});
		synth.Voice(newVoice);
		if (found) result->Success(1);
		else result->Success(0);
	}


	FlutterTtsPlugin::FlutterTtsPlugin() {
		synth = SpeechSynthesizer();
		addMplayer();
		isPaused = false;
		isSpeaking = false;
		awaitSpeakCompletion = false;
		speakResult = FlutterResult();
	}

	FlutterTtsPlugin::~FlutterTtsPlugin() { synthesisState->alive = false; mPlayer.Close(); }

	void FlutterTtsPlugin::HandleMethodCall(
		const flutter::MethodCall<flutter::EncodableValue>& method_call,
		FlutterResult result) {
		if (method_call.method_name().compare("getPlatformVersion") == 0) {
			std::ostringstream version_stream;
			version_stream << "Windows UWP";
			result->Success(flutter::EncodableValue(version_stream.str()));
		}

#else
#include <string>
#include <atlstr.h>
#include <array>
#include <sapi.h>
#pragma warning(disable:4996)
#include <sphelper.h>
#pragma warning(default: 4996)
namespace {

	class FlutterTtsPlugin : public flutter::Plugin {
	public:
		static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);
		FlutterTtsPlugin();
		virtual ~FlutterTtsPlugin();
	private:
		// Called when a method is called on this plugin's channel from Dart.
		void HandleMethodCall(
			const flutter::MethodCall<flutter::EncodableValue>& method_call,
			std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

		void speak(const std::string, FlutterResult);
		std::function<void()> synthesisWriter(const std::string&, const std::filesystem::path&);
		bool awaitSynthCompletion = false;
		std::shared_ptr<SynthesisState> synthesisState = std::make_shared<SynthesisState>();
		void pause();
		void continuePlay();
		void stop();
		void setVolume(const double);
		void setPitch(const double);
		void setRate(const double);
		void getVoices(flutter::EncodableList&);
		void setVoice(const std::string, const std::string, FlutterResult&);
		void getLanguages(flutter::EncodableList&);
		void setLanguage(const std::string, FlutterResult&);

		ISpVoice* pVoice;
		bool awaitSpeakCompletion = false;
		bool isPaused;
		double pitch;
		bool speaking();
		bool paused();
		FlutterResult speakResult;
    	HANDLE addWaitHandle;
	};

	void FlutterTtsPlugin::RegisterWithRegistrar(
		flutter::PluginRegistrarWindows* registrar) {
		methodChannel =
			std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
				registrar->messenger(), "flutter_tts",
				&flutter::StandardMethodCodec::GetInstance());
		auto plugin = std::make_unique<FlutterTtsPlugin>();
		methodChannel->SetMethodCallHandler(
			[plugin_pointer = plugin.get()](const auto& call, auto result) {
			plugin_pointer->HandleMethodCall(call, std::move(result));
		});

		registrar->AddPlugin(std::move(plugin));
	}

	FlutterTtsPlugin::FlutterTtsPlugin() {
		addWaitHandle = NULL;
		isPaused = false;
		speakResult = NULL;
		pVoice = NULL;
		HRESULT hr;
		hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
		if (FAILED(hr))
		{
			throw std::exception("TTS init failed");
		}

		hr = CoCreateInstance(CLSID_SpVoice, NULL, CLSCTX_ALL, IID_ISpVoice, (void**)&pVoice);
		if (FAILED(hr))
		{
			throw std::exception("TTS create instance failed");
		}
		pitch = 0;
	}

	FlutterTtsPlugin::~FlutterTtsPlugin() {
		synthesisState->alive = false;
		::CoUninitialize();
	}

    void CALLBACK setResult(PVOID lpParam, BOOLEAN TimerOrWaitFired)
    {
        flutter::MethodResult<flutter::EncodableValue>* p = (flutter::MethodResult<flutter::EncodableValue>*) lpParam;
        p->Success(1);
    }

    void CALLBACK onCompletion(PVOID lpParam, BOOLEAN TimerOrWaitFired)
    {
        methodChannel->InvokeMethod("speak.onComplete", NULL);
    }

	bool FlutterTtsPlugin::speaking()
	{
		SPVOICESTATUS status;
		pVoice->GetStatus(&status, NULL);
		if (status.dwRunningState == SPRS_IS_SPEAKING) return true;
		return false;
	}
	bool FlutterTtsPlugin::paused() { return isPaused; }


	void FlutterTtsPlugin::speak(const std::string text, FlutterResult result) {
		HRESULT hr;
		const std::string arg = "<PITCH MIDDLE = '" + std::to_string(int((pitch - 1) * 10 * (1 + (pitch < 1)) )) + "'/>" + text;

		int wchars_num = MultiByteToWideChar(CP_UTF8, 0, arg.c_str(), -1, NULL, 0);
		wchar_t* wstr = new wchar_t[wchars_num];
		MultiByteToWideChar(CP_UTF8, 0, arg.c_str(), -1, wstr, wchars_num);
		hr = pVoice->Speak(wstr, 1, NULL);
		delete[] wstr;
		HANDLE speakCompletionHandle = pVoice->SpeakCompleteEvent();
		methodChannel->InvokeMethod("speak.onStart", NULL);
		RegisterWaitForSingleObject(&addWaitHandle, speakCompletionHandle, (WAITORTIMERCALLBACK)&onCompletion, speakResult.get(), INFINITE, WT_EXECUTEONLYONCE);
		if (awaitSpeakCompletion){
		    speakResult = std::move(result);
		    RegisterWaitForSingleObject(&addWaitHandle, speakCompletionHandle, (WAITORTIMERCALLBACK)&setResult, speakResult.get(), INFINITE, WT_EXECUTEONLYONCE);
		}
		else result->Success(1);
	}
	std::function<void()> FlutterTtsPlugin::synthesisWriter(
		const std::string& text, const std::filesystem::path& path) {
		CComPtr<ISpObjectToken> token;
		winrt::check_hresult(pVoice->GetVoice(&token));
		CComHeapPtr<wchar_t> tokenId;
		winrt::check_hresult(token->GetId(&tokenId));
		const std::wstring voiceId(tokenId.m_pData);
		USHORT volume;
		long rate;
		winrt::check_hresult(pVoice->GetVolume(&volume));
		winrt::check_hresult(pVoice->GetRate(&rate));
		// SAPI 使用 XML 设置音调，正文必须转义以保留 &、< 等字符。
		std::string escaped;
		for (const char ch : text) {
			if (ch == '&') escaped += "&amp;";
			else if (ch == '<') escaped += "&lt;";
			else if (ch == '>') escaped += "&gt;";
			else escaped += ch;
		}
		const auto xml = winrt::to_hstring("<pitch middle='" +
			std::to_string(int((pitch - 1) * 10 * (1 + (pitch < 1)))) + "'>" + escaped + "</pitch>");
		return [path, voiceId, volume, rate, xml]() {
			// COM 对象在工作线程创建和释放，避免跨 apartment 使用 pVoice。
			CComPtr<ISpVoice> voice;
			CComPtr<ISpObjectToken> voiceToken;
			CComPtr<ISpStream> stream;
			winrt::check_hresult(voice.CoCreateInstance(CLSID_SpVoice));
			winrt::check_hresult(SpGetTokenFromId(voiceId.c_str(), &voiceToken));
			winrt::check_hresult(voice->SetVoice(voiceToken));
			winrt::check_hresult(voice->SetVolume(volume));
			winrt::check_hresult(voice->SetRate(rate));
			CSpStreamFormat format;
			winrt::check_hresult(format.AssignFormat(SPSF_22kHz16BitMono));
			winrt::check_hresult(SPBindToFile(path.c_str(), SPFM_CREATE_ALWAYS,
				&stream, &format.FormatId(), format.WaveFormatExPtr()));
			winrt::check_hresult(voice->SetOutput(stream, TRUE));
			winrt::check_hresult(voice->Speak(xml.c_str(), SPF_IS_XML, nullptr));
			winrt::check_hresult(voice->SetOutput(nullptr, FALSE));
			// Close 完成 WAV 头部写入后，才能报告合成成功。
			winrt::check_hresult(stream->Close());
		};
	}

	void FlutterTtsPlugin::pause()
	{
		if (isPaused == false)
		{
			pVoice->Pause();
			isPaused = true;
		}
	    methodChannel->InvokeMethod("speak.onPause", NULL);
	}
	void FlutterTtsPlugin::continuePlay()
	{
		isPaused = false;
		pVoice->Resume();
	    methodChannel->InvokeMethod("speak.onContinue", NULL);
	}
	void FlutterTtsPlugin::stop()
	{
		pVoice->Speak(L"", 2, NULL);
		pVoice->Resume();
		isPaused = false;
	    methodChannel->InvokeMethod("speak.onCancel", NULL);
	}
	void FlutterTtsPlugin::setVolume(const double newVolume)
	{
		const USHORT volume = (short)(100 * newVolume);
		pVoice->SetVolume(volume);
	}
	void FlutterTtsPlugin::setPitch(const double newPitch) {pitch = newPitch;}
	void FlutterTtsPlugin::setRate(const double newRate)
	{
		const long speechRate = (long)((newRate - 0.5) * 15);
		pVoice->SetRate(speechRate);
	}
	void FlutterTtsPlugin::getVoices(flutter::EncodableList& voices) {
		HRESULT hr;
		IEnumSpObjectTokens* cpEnum = NULL;
		hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
		if (FAILED(hr)) return;

 		ULONG ulCount = 0;
		// Get the number of voices.
		hr = cpEnum->GetCount(&ulCount);
		if (FAILED(hr)) return;
		ISpObjectToken* cpVoiceToken = NULL;
		while (ulCount--)
		{
			cpVoiceToken = NULL;
			hr = cpEnum->Next(1, &cpVoiceToken, NULL);
			if (FAILED(hr)) return;
			CComPtr<ISpDataKey> cpAttribKey;
			hr = cpVoiceToken->OpenKey(L"Attributes", &cpAttribKey);
			if (FAILED(hr)) return;
			WCHAR* psz = NULL;
			hr = cpAttribKey->GetStringValue(L"Language", &psz);
		    wchar_t locale[25];
            LCIDToLocaleName((LCID)std::strtol(CW2A(psz), NULL, 16), locale, 25, 0);
            ::CoTaskMemFree(psz);
            std::string language = CW2A(locale);
            psz = NULL;
            cpAttribKey->GetStringValue(L"Name", &psz);
			std::string name = CW2A(psz);
			::CoTaskMemFree(psz);
            flutter::EncodableMap voiceInfo;
            voiceInfo[flutter::EncodableValue("locale")] = language;
            voiceInfo[flutter::EncodableValue("name")] = name;
            voices.push_back(flutter::EncodableMap(voiceInfo));
			cpVoiceToken->Release();
		}
	}
	void FlutterTtsPlugin::setVoice(const std::string voiceLanguage, const std::string voiceName, FlutterResult& result) {
		HRESULT hr;
		IEnumSpObjectTokens* cpEnum = NULL;
		hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
		if (FAILED(hr)) { result->Success(0); return; }
		ULONG ulCount = 0;
		hr = cpEnum->GetCount(&ulCount);
		if (FAILED(hr)) { result->Success(0); return; }
		ISpObjectToken* cpVoiceToken = NULL;
		bool success = false;
		while (ulCount--)
		{
			cpVoiceToken = NULL;
			hr = cpEnum->Next(1, &cpVoiceToken, NULL);
			if (FAILED(hr)) { result->Success(0); return; }
			CComPtr<ISpDataKey> cpAttribKey;
			hr = cpVoiceToken->OpenKey(L"Attributes", &cpAttribKey);
			if (FAILED(hr)) { result->Success(0); return; }
			WCHAR* psz = NULL;
			hr = cpAttribKey->GetStringValue(L"Name", &psz);
			if (FAILED(hr)) { result->Success(0); return; }
			std::string name = CW2A(psz);
			::CoTaskMemFree(psz);
			psz = NULL;
			hr = cpAttribKey->GetStringValue(L"Language", &psz);
		    wchar_t locale[25];
            LCIDToLocaleName((LCID)std::strtol(CW2A(psz), NULL, 16), locale, 25, 0);
            ::CoTaskMemFree(psz);
            std::string language = CW2A(locale);
			if (name == voiceName && language == voiceLanguage)
			{
				pVoice->SetVoice(cpVoiceToken);
				success = true;
			}
			cpVoiceToken->Release();
		}
		result->Success(success ? 1 : 0);
	}
	void FlutterTtsPlugin::getLanguages(flutter::EncodableList& languages)
	{
		HRESULT hr;
		IEnumSpObjectTokens* cpEnum = NULL;
		hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
		if (FAILED(hr)) return;

 		ULONG ulCount = 0;
		// Get the number of voices.
		hr = cpEnum->GetCount(&ulCount);
		if (FAILED(hr)) return;
		ISpObjectToken* cpVoiceToken = NULL;
        std::set<flutter::EncodableValue> languagesSet = {};
		while (ulCount--)
		{
			cpVoiceToken = NULL;
			hr = cpEnum->Next(1, &cpVoiceToken, NULL);
			if (FAILED(hr)) return;
			CComPtr<ISpDataKey> cpAttribKey;
			hr = cpVoiceToken->OpenKey(L"Attributes", &cpAttribKey);
			if (FAILED(hr)) return;

			WCHAR* psz = NULL;
			hr = cpAttribKey->GetStringValue(L"Language", &psz);
		    wchar_t locale[25];
            LCIDToLocaleName((LCID)std::strtol(CW2A(psz), NULL, 16), locale, 25, 0);
            std::string language = CW2A(locale);
			languagesSet.insert(flutter::EncodableValue(language));
			::CoTaskMemFree(psz);
			cpVoiceToken->Release();
		}
        std::for_each(begin(languagesSet), end(languagesSet), [&languages](const flutter::EncodableValue value)
            {
                languages.push_back(value);
            });
	}

	void FlutterTtsPlugin::setLanguage(const std::string voiceLanguage, FlutterResult& result) {
		HRESULT hr;
		IEnumSpObjectTokens* cpEnum = NULL;
		hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &cpEnum);
		if (FAILED(hr)) { result->Success(0); return; }
		ULONG ulCount = 0;
		hr = cpEnum->GetCount(&ulCount);
		if (FAILED(hr)) { result->Success(0); return; }
		ISpObjectToken* cpVoiceToken = NULL;
		bool found = false;
		while (ulCount--)
		{
			cpVoiceToken = NULL;
			hr = cpEnum->Next(1, &cpVoiceToken, NULL);
			if (FAILED(hr)) { result->Success(0); return; }
			CComPtr<ISpDataKey> cpAttribKey;
			hr = cpVoiceToken->OpenKey(L"Attributes", &cpAttribKey);
			if (FAILED(hr)) { result->Success(0); return; }

			WCHAR* psz = NULL;
			hr = cpAttribKey->GetStringValue(L"Language", &psz);
		    wchar_t locale[25];
            LCIDToLocaleName((LCID)std::strtol(CW2A(psz), NULL, 16), locale, 25, 0);
            std::string language = CW2A(locale);
			if (language == voiceLanguage)
			{
				pVoice->SetVoice(cpVoiceToken);
				found = true;
			}
			::CoTaskMemFree(psz);
			cpVoiceToken->Release();
		}
		if (found) result->Success(1);
		else result->Success(0);
	}


	void FlutterTtsPlugin::HandleMethodCall(
		const flutter::MethodCall<flutter::EncodableValue>& method_call,
		FlutterResult result) {

		if (method_call.method_name().compare("getPlatformVersion") == 0) {
			std::ostringstream version_stream;
			version_stream << "Windows ";
			if (IsWindows10OrGreater()) {
				version_stream << "10+";
			}
			else if (IsWindows8OrGreater()) {
				version_stream << "8";
			}
			else if (IsWindows7OrGreater()) {
				version_stream << "7";
			}
			result->Success(flutter::EncodableValue(version_stream.str()));
		}
#endif
		else if (method_call.method_name() == "awaitSynthCompletion") {
			const auto* args = method_call.arguments();
			if (args && std::holds_alternative<bool>(*args)) {
				awaitSynthCompletion = std::get<bool>(*args);
				result->Success(1);
			}
			else result->Success(0);
		}
		else if (method_call.method_name() == "synthesizeToFile") {
			const auto* args = method_call.arguments();
			const auto* map = args ? std::get_if<flutter::EncodableMap>(args) : nullptr;
			if (!map || synthesisState->active) { result->Success(0); return; }
			const auto textIt = map->find(flutter::EncodableValue("text"));
			const auto fileIt = map->find(flutter::EncodableValue("fileName"));
			const auto fullIt = map->find(flutter::EncodableValue("isFullPath"));
			if (textIt == map->end() || fileIt == map->end() ||
				!std::holds_alternative<std::string>(textIt->second) ||
				!std::holds_alternative<std::string>(fileIt->second) ||
				(fullIt != map->end() && !std::holds_alternative<bool>(fullIt->second))) {
				result->Success(0); return;
			}
			const auto& text = std::get<std::string>(textIt->second);
			const auto& fileName = std::get<std::string>(fileIt->second);
			if (text.empty() || fileName.empty() || text.find('\0') != std::string::npos ||
				fileName.find('\0') != std::string::npos) { result->Success(0); return; }
			try {
				auto path = std::filesystem::u8path(fileName);
				const bool fullPath = fullIt != map->end() && std::get<bool>(fullIt->second);
				if (fullPath != path.is_absolute() || !path.has_filename()) {
					result->Success(0); return;
				}
				path = std::filesystem::absolute(path);
				auto writer = synthesisWriter(text, path);
				winrt::apartment_context caller;
				synthesisState->active = true;
				methodChannel->InvokeMethod("synth.onStart", nullptr);
				if (!awaitSynthCompletion) { result->Success(1); result.reset(); }
				writeSynthesis(std::move(writer), synthesisState, std::move(result), caller);
			}
			catch (const winrt::hresult_error& e) {
				synthesisState->active = false;
				methodChannel->InvokeMethod("synth.onError",
					std::make_unique<flutter::EncodableValue>(winrt::to_string(e.message())));
				if (result) result->Success(0);
			}
			catch (const std::exception& e) {
				synthesisState->active = false;
				methodChannel->InvokeMethod("synth.onError",
					std::make_unique<flutter::EncodableValue>(e.what()));
				if (result) result->Success(0);
			}
		}
		else if (method_call.method_name().compare("awaitSpeakCompletion") == 0) {
            const flutter::EncodableValue arg = method_call.arguments()[0];
            if (std::holds_alternative<bool>(arg)) {
                awaitSpeakCompletion = std::get<bool>(arg);
                result->Success(1);
            }
            else result->Success(0);
        }
		else if (method_call.method_name().compare("speak") == 0) {
			if (isPaused) { continuePlay(); result->Success(1); return; }
			const flutter::EncodableValue arg = method_call.arguments()[0];
			if (std::holds_alternative<std::string>(arg)) {
				if (!speaking()) {
					const std::string text = std::get<std::string>(arg);
					speak(text, std::move(result));
				}
				else result->Success(0);
			}
			else result->Success(0);
		}
		else if (method_call.method_name().compare("pause") == 0) {
			FlutterTtsPlugin::pause();
			result->Success(1);
		}
		else if (method_call.method_name().compare("setLanguage") == 0) {
			const flutter::EncodableValue arg = method_call.arguments()[0];
			if (std::holds_alternative<std::string>(arg)) {
				const std::string lang = std::get<std::string>(arg);
				setLanguage(lang, result);
			}
			else result->Success(0);
		}
		else if (method_call.method_name().compare("setVolume") == 0) {
			const flutter::EncodableValue arg = method_call.arguments()[0];
			if (std::holds_alternative<double>(arg)) {
				const double newVolume = std::get<double>(arg);
				setVolume(newVolume);
				result->Success(1);
			}
			else result->Success(0);

		}
		else if (method_call.method_name().compare("setSpeechRate") == 0) {
			const flutter::EncodableValue arg = method_call.arguments()[0];
			if (std::holds_alternative<double>(arg)) {
				const double newRate = std::get<double>(arg);
				setRate(newRate);
				result->Success(1);
			}
			else result->Success(0);

		}
        else if (method_call.method_name().compare("setPitch") == 0) {
            const flutter::EncodableValue arg = method_call.arguments()[0];
            if (std::holds_alternative<double>(arg)) {
                const double newPitch = std::get<double>(arg);
                setPitch(newPitch);
                result->Success(1);
            }
            else result->Success(0);
        }
		else if (method_call.method_name().compare("setVoice") == 0) {
			const flutter::EncodableValue arg = method_call.arguments()[0];
			if (std::holds_alternative<flutter::EncodableMap>(arg)) {
				const flutter::EncodableMap voiceInfo = std::get<flutter::EncodableMap>(arg);
				std::string voiceLanguage = "";
				std::string voiceName = "";
				auto voiceLanguage_it = voiceInfo.find(flutter::EncodableValue("locale"));
				if (voiceLanguage_it != voiceInfo.end()) voiceLanguage = std::get<std::string>(voiceLanguage_it->second);
				auto voiceName_it = voiceInfo.find(flutter::EncodableValue("name"));
				if (voiceName_it != voiceInfo.end()) voiceName = std::get<std::string>(voiceName_it->second);
				setVoice(voiceLanguage, voiceName, result);
			}
			else result->Success(0);
		}
		else if (method_call.method_name().compare("stop") == 0) {
			stop();
			result->Success(1);
		}
		else if (method_call.method_name().compare("getLanguages") == 0) {
			flutter::EncodableList l;
			getLanguages(l);
			result->Success(l);
		}
		else if (method_call.method_name().compare("getVoices") == 0) {
			flutter::EncodableList l;
			getVoices(l);
			result->Success(l);
		}
		else {
			result->NotImplemented();
		}
	}
}

void FlutterTtsPluginRegisterWithRegistrar(
	FlutterDesktopPluginRegistrarRef registrar) {
	FlutterTtsPlugin::RegisterWithRegistrar(
		flutter::PluginRegistrarManager::GetInstance()
		->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
