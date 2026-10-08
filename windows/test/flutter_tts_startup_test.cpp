#include "../flutter_tts_plugin.cpp"

#include <iostream>
#include <roapi.h>
#include <winstring.h>

namespace {
int activationCalls = 0;
int speechActivationCalls = 0;
int playerActivationCalls = 0;
bool rejectSpeech = false;
bool rejectPlayer = false;

int32_t __stdcall Activate(void* classId, winrt::guid const& iid, void** factory) noexcept {
  ++activationCalls;
  const auto name = WindowsGetStringRawBuffer(static_cast<HSTRING>(classId), nullptr);
  if (wcscmp(name, L"Windows.Media.SpeechSynthesis.SpeechSynthesizer") == 0) {
    ++speechActivationCalls;
    if (rejectSpeech) { *factory = nullptr; return E_ACCESSDENIED; }
  }
  if (wcscmp(name, L"Windows.Media.Playback.MediaPlayer") == 0) {
    ++playerActivationCalls;
    if (rejectPlayer) { *factory = nullptr; return REGDB_E_CLASSNOTREG; }
  }
  return RoGetActivationFactory(static_cast<HSTRING>(classId),
    reinterpret_cast<const IID&>(iid), factory);
}

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Response {
  int replies = 0;
  bool succeeded = false;
  bool notImplemented = false;
  flutter::EncodableValue value;
  std::string errorCode;
  std::string errorMessage;
  flutter::EncodableValue details;
};

class TestResult : public flutter::MethodResult<flutter::EncodableValue> {
 public:
  explicit TestResult(std::shared_ptr<Response> response) : response_(std::move(response)) {}
 protected:
  void SuccessInternal(const flutter::EncodableValue* value) override {
    ++response_->replies;
    response_->succeeded = true;
    if (value) response_->value = *value;
  }
  void ErrorInternal(const std::string& code, const std::string& message,
    const flutter::EncodableValue* details) override {
    ++response_->replies;
    response_->errorCode = code;
    response_->errorMessage = message;
    if (details) response_->details = *details;
  }
  void NotImplementedInternal() override {
    ++response_->replies;
    response_->notImplemented = true;
  }
 private:
  std::shared_ptr<Response> response_;
};

class TestMessenger : public flutter::BinaryMessenger {
 public:
  mutable std::vector<std::string> events;
  void Send(const std::string&, const uint8_t* message, size_t size,
    flutter::BinaryReply = nullptr) const override {
    const auto call = flutter::StandardMethodCodec::GetInstance().DecodeMethodCall(message, size);
    if (call) events.push_back(call->method_name());
  }
  void SetMessageHandler(const std::string&, flutter::BinaryMessageHandler) override {}
};

class FlutterTtsPluginTest {
 public:
  static std::shared_ptr<Response> Call(FlutterTtsPlugin& plugin,
    const std::string& method, flutter::EncodableValue arguments = {}) {
    auto response = std::make_shared<Response>();
    flutter::MethodCall<flutter::EncodableValue> call(method,
      std::make_unique<flutter::EncodableValue>(std::move(arguments)));
    plugin.HandleMethodCall(call, std::make_unique<TestResult>(response));
    return response;
  }
  static bool SynthesisActive(const FlutterTtsPlugin& plugin) {
    return plugin.synthesisState->active;
  }
  static double Volume(const FlutterTtsPlugin& plugin) {
    return plugin.synth.Options().AudioVolume();
  }
  static bool BackendInitialized(const FlutterTtsPlugin& plugin) {
    return static_cast<bool>(plugin.synth) || static_cast<bool>(plugin.mPlayer);
  }
  static void CreatePlayer(FlutterTtsPlugin& plugin) { plugin.addMplayer(); }
};

struct Fixture {
  TestMessenger messenger;
  std::unique_ptr<FlutterTtsPlugin> plugin;
  Fixture() {
    methodChannel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      &messenger, "flutter_tts", &flutter::StandardMethodCodec::GetInstance());
    plugin = std::make_unique<FlutterTtsPlugin>();
  }
  ~Fixture() { plugin.reset(); methodChannel.reset(); }
  std::shared_ptr<Response> Call(const std::string& method, flutter::EncodableValue arguments = {}) {
    return FlutterTtsPluginTest::Call(*plugin, method, std::move(arguments));
  }
};

void CheckSuccess(const Response& response) {
  Check(response.replies == 1 && response.succeeded, "Expected exactly one successful reply");
}

void CheckInitError(const Response& response, const char* stage, const char* hresult) {
  Check(response.replies == 1 && response.errorCode == "tts_init_failed",
    "Expected exactly one initialization error reply");
  const auto& details = std::get<flutter::EncodableMap>(response.details);
  Check(std::get<std::string>(details.at(flutter::EncodableValue("stage"))) == stage,
    "Wrong initialization stage");
  Check(std::get<std::string>(details.at(flutter::EncodableValue("hresult"))) == hresult,
    "Initialization HRESULT was lost");
  Check(response.errorMessage.find(hresult) != std::string::npos, "Diagnostic message lost HRESULT");
}

void Run(const std::string& scenario) {
  if (scenario == "startup") {
    rejectSpeech = rejectPlayer = true;
    {
      Fixture fixture;
      Check(!FlutterTtsPluginTest::BackendInitialized(*fixture.plugin), "Registration initialized a backend");
    }
    Check(activationCalls == 0, "Plugin construction/destruction activated a Windows backend");
  } else if (scenario == "controls") {
    rejectSpeech = rejectPlayer = true;
    Fixture fixture;
    CheckSuccess(*fixture.Call("getPlatformVersion"));
    CheckSuccess(*fixture.Call("awaitSynthCompletion", true));
    CheckSuccess(*fixture.Call("awaitSpeakCompletion", true));
    CheckSuccess(*fixture.Call("pause"));
    CheckSuccess(*fixture.Call("stop"));
    CheckSuccess(*fixture.Call("stop"));
    const auto unsupported = fixture.Call("unsupported");
    Check(unsupported->replies == 1 && unsupported->notImplemented, "Unknown method was not rejected");
    Check(activationCalls == 0, "Metadata, completion settings, pause or stop activated a backend");
    Check(!FlutterTtsPluginTest::BackendInitialized(*fixture.plugin), "Non-TTS calls initialized a backend");
  } else if (scenario == "speech_failure") {
    rejectSpeech = true;
    Fixture fixture;
    CheckInitError(*fixture.Call("getVoices"), "speech_synthesizer", "0x80070005");
    CheckInitError(*fixture.Call("synthesizeToFile"), "speech_synthesizer", "0x80070005");
    Check(speechActivationCalls == 2 && playerActivationCalls == 0,
      "Failed activation was cached or unnecessarily activated a player");
    rejectSpeech = false;
    const auto voices = fixture.Call("getVoices");
    CheckSuccess(*voices);
    Check(!std::get<flutter::EncodableList>(voices->value).empty(), "No installed voices available");
    CheckSuccess(*fixture.Call("setVolume", 0.8));
    CheckSuccess(*fixture.Call("getVoices"));
    Check(std::abs(FlutterTtsPluginTest::Volume(*fixture.plugin) - 0.8) < 0.001,
      "Retry or repeated voice query discarded the configured synthesizer");
  } else if (scenario == "media_failure") {
    rejectPlayer = true;
    Fixture fixture;
    CheckInitError(*fixture.Call("speak", "Hello"), "media_player", "0x80040154");
    CheckInitError(*fixture.Call("speak", "Hello again"), "media_player", "0x80040154");
    CheckSuccess(*fixture.Call("getVoices"));
    CheckSuccess(*fixture.Call("stop"));
    Check(playerActivationCalls == 2, "Stop unnecessarily retried failed player activation");
  } else if (scenario == "voice_query") {
    rejectPlayer = true;
    Fixture fixture;
    CheckSuccess(*fixture.Call("getVoices"));
    CheckSuccess(*fixture.Call("getLanguages"));
    CheckSuccess(*fixture.Call("setVolume", 0.8));
    CheckSuccess(*fixture.Call("setPitch", 1.0));
    CheckSuccess(*fixture.Call("setSpeechRate", 0.5));
    Check(playerActivationCalls == 0, "Voice/settings operations activated a player");
  } else if (scenario == "player_stop") {
    Fixture fixture;
    FlutterTtsPluginTest::CreatePlayer(*fixture.plugin);
    const auto initialCalls = playerActivationCalls;
    rejectPlayer = true;
    CheckSuccess(*fixture.Call("stop"));
    CheckSuccess(*fixture.Call("stop"));
    Check(playerActivationCalls == initialCalls, "Stop recreated a player and required fresh activation");
  } else if (scenario == "synthesis") {
    rejectPlayer = true;
    Fixture fixture;
    const auto file = std::filesystem::temp_directory_path() /
      (L"flutter_tts_startup_test_" + std::to_wstring(GetCurrentProcessId()) + L".wav");
    struct DeleteFile {
      std::filesystem::path path;
      ~DeleteFile() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{file};
    const auto response = fixture.Call("synthesizeToFile", flutter::EncodableMap{
      {flutter::EncodableValue("text"), flutter::EncodableValue("你好，Windows！ Hello & <world>.")},
      {flutter::EncodableValue("fileName"), flutter::EncodableValue(winrt::to_string(file.wstring()))},
      {flutter::EncodableValue("isFullPath"), flutter::EncodableValue(true)},
    });
    CheckSuccess(*response);
    const auto deadline = GetTickCount64() + 15000;
    while (FlutterTtsPluginTest::SynthesisActive(*fixture.plugin) && GetTickCount64() < deadline) {
      MSG message;
      while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
      }
      MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    Check(!FlutterTtsPluginTest::SynthesisActive(*fixture.plugin), "File synthesis timed out");
    Check(std::count(fixture.messenger.events.begin(), fixture.messenger.events.end(), "synth.onComplete") == 1,
      "File synthesis did not report completion exactly once");
    Check(std::count(fixture.messenger.events.begin(), fixture.messenger.events.end(), "synth.onError") == 0,
      "File synthesis reported an error");
    std::ifstream audio(file, std::ios::binary);
    char header[12]{};
    audio.read(header, sizeof(header));
    Check(audio.gcount() == sizeof(header) && std::string(header, 4) == "RIFF" &&
      std::string(header + 8, 4) == "WAVE", "Synthesis did not produce a WAV file");
    Check(playerActivationCalls == 0, "File synthesis required a MediaPlayer");
  } else {
    throw std::runtime_error("Unknown test scenario");
  }
}
}

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "startup";
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  winrt_activation_handler = Activate;
  bool passed = false;
  try {
    Run(scenario);
    passed = true;
  } catch (const winrt::hresult_error& e) {
    std::cerr << "Unhandled WinRT error: " << winrt::to_string(e.message()) << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
  }
  winrt_activation_handler = nullptr;
  winrt::clear_factory_cache();
  winrt::uninit_apartment();
  return passed ? 0 : 1;
}
