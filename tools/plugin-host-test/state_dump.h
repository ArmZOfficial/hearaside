// Included inside main.cpp's anonymous namespace to reuse its JUCE hosting and state wrappers.

void setDumpEnvironment(const char* key, const juce::String& value) {
#if JUCE_WINDOWS
    _putenv_s(key, value.toRawUTF8());
#else
    setenv(key, value.toRawUTF8(), 1);
#endif
}

int dumpStates(const juce::String& path, const juce::String& outputPath) {
    const juce::File output(outputPath);
    if (output.exists() && (!output.isDirectory() || !output.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty())) {
        std::fprintf(stderr, "State dump requires an empty output directory\n");
        return 2;
    }
    const auto isolation = "CompatState" + juce::Uuid().toString().removeCharacters("-");
    setDumpEnvironment("HEARASIDE_DEFAULT_BUS", isolation);
    setDumpEnvironment("HEARASIDE_SETTINGS_NAME", isolation);
    setDumpEnvironment("HEARASIDE_ACCOUNT_NAME", isolation);
    setDumpEnvironment("HEARASIDE_NO_TUNNEL", "1");
    setDumpEnvironment("HEARASIDE_API_URL", "http://127.0.0.1:9");

    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager(fm);
    auto processor = load(fm, bundlePath(path));
    if (!processor) return 1;
    const auto description = processor->getPluginDescription();
    const bool track = description.name == "HEARASIDE Track";
    const bool hub = description.name == "HEARASIDE Hub";
    const bool app = description.name == "HEARASIDE App Audio";
    if (!track && !hub && !app) {
        std::fprintf(stderr, "State dump supports HEARASIDE Track, Hub and App Audio only\n");
        return 2;
    }
    juce::MemoryBlock defaults;
    processor->getStateInformation(defaults);
    if (!output.createDirectory().wasOk()) return 1;

    auto* manifest = new juce::DynamicObject();
    const juce::var manifestVar(manifest);
    manifest->setProperty("schemaVersion", 1);
    manifest->setProperty("pluginName", description.name);
    manifest->setProperty("pluginVersion", description.version);
    manifest->setProperty("pluginFormat", description.pluginFormatName);
    manifest->setProperty("uniqueId", description.uniqueId);
    manifest->setProperty("uniqueIdHex", juce::String::toHexString(juce::uint32(description.uniqueId)));
    manifest->setProperty("deprecatedUid", description.deprecatedUid);
    manifest->setProperty("deprecatedUidHex", juce::String::toHexString(juce::uint32(description.deprecatedUid)));
    manifest->setProperty("manufacturer", description.manufacturerName);
    manifest->setProperty("category", description.category);
    manifest->setProperty("parameterAddressing", description.pluginFormatName == "VST"
        ? "zero-based VST2 parameter index" : "JUCE host parameter index; native VST3 IDs captured separately");
    manifest->setProperty("parameterRangeLimitation", "JUCE host API exposes normalized values and text, not a raw plain-value float range");
    manifest->setProperty("sourcePath", juce::File(path).getFullPathName());
    manifest->setProperty("stateContainer", description.pluginFormatName == "VST3" ? "JUCE host VST3 wrapper" : "VST2 FXB bank");
    manifest->setProperty("color", "Host channel metadata; not serialized by HEARASIDE state");
    juce::Array<juce::var> cases;
    const juce::String labels[] = { "default", "parameters-changed", "parameters-minimum", "parameters-maximum",
        "metadata", track ? "friend-input" : hub ? "friends-and-pairs" : "friend-source" };

    for (int scenario = 0; scenario < 6; ++scenario) {
        processor->setStateInformation(defaults.getData(), int(defaults.getSize()));
        if (scenario >= 1 && scenario <= 3) {
            for (auto* parameter : processor->getParameters()) {
                const float value = scenario == 2 ? 0.0f : scenario == 3 ? 1.0f
                    : parameter->getDefaultValue() < 0.5f ? 1.0f : 0.0f;
                parameter->setValueNotifyingHost(value);
            }
        }
        if (scenario >= 4) {
            HubState edited(*processor);
            if (!edited.plugin) return 1;
            auto& xml = *edited.plugin;
            if (track) {
                xml.setAttribute("name", juce::String::fromUTF8("เสียงร้อง ทดสอบ"));
                xml.setAttribute("stem", 3);
                xml.setAttribute("chainMs", 27.5);
                xml.setAttribute("role", scenario == 5 ? "friend" : "track");
                xml.setAttribute("friendId", scenario == 5 ? 142 : 0);
            } else if (hub) {
                xml.setAttribute("lineUp", 0);
                auto* stems = xml.getChildByName("STEMS");
                if (stems == nullptr) stems = xml.createNewChildElement("STEMS");
                stems->setAttribute("s0", juce::String::fromUTF8("เสียงร้อง"));
                stems->setAttribute("s7", "Backing & keys");
                if (scenario == 5) {
                    xml.deleteAllChildElementsWithTagName("FRIENDS");
                    auto* friends = xml.createNewChildElement("FRIENDS");
                    friends->setAttribute("nextId", 143);
                    auto* friendEntry = friends->createNewChildElement("FRIEND");
                    friendEntry->setAttribute("id", 142);
                    friendEntry->setAttribute("name", juce::String::fromUTF8("เพื่อนทดสอบ"));
                    friendEntry->setAttribute("token", "compatfixturefriendtoken142");
                    friendEntry->setAttribute("vol", -6.0);
                    friendEntry->setAttribute("pan", -0.25);
                    friendEntry->setAttribute("mon", 1);
                    friendEntry->setAttribute("str", 0);
                    friendEntry->setAttribute("solo", 1);
                    xml.deleteAllChildElementsWithTagName("PAIRS");
                    auto* pair = xml.createNewChildElement("PAIRS")->createNewChildElement("PAIR");
                    pair->setAttribute("feeder", "Fixture feeder");
                    pair->setAttribute("slot", "fixture-track-uuid");
                }
            } else {
                xml.setAttribute("follow", 1);
                xml.setAttribute("app", scenario == 5 ? "*friend:142*" : "*link*");
            }
            edited.load(*processor);
        }

        HubState captured(*processor);
        if (!captured.plugin) return 1;
        const auto& xml = *captured.plugin;
        if (scenario >= 4) {
            bool metadataOk = false;
            if (track) metadataOk = xml.getStringAttribute("name") == juce::String::fromUTF8("เสียงร้อง ทดสอบ")
                && xml.getIntAttribute("stem") == 3 && std::abs(xml.getDoubleAttribute("chainMs") - 27.5) < 0.0001
                && xml.getStringAttribute("role") == (scenario == 5 ? "friend" : "track")
                && xml.getIntAttribute("friendId") == (scenario == 5 ? 142 : 0);
            if (hub) {
                const auto* stems = xml.getChildByName("STEMS");
                metadataOk = !xml.getBoolAttribute("lineUp", true) && stems
                    && stems->getStringAttribute("s0") == juce::String::fromUTF8("เสียงร้อง")
                    && stems->getStringAttribute("s7") == "Backing & keys";
                if (scenario == 5) {
                    const auto* friends = xml.getChildByName("FRIENDS");
                    const auto* pairs = xml.getChildByName("PAIRS");
                    metadataOk = metadataOk && friends && friends->getNumChildElements() == 1
                        && friends->getChildElement(0)->getIntAttribute("id") == 142
                        && pairs && pairs->getNumChildElements() == 1
                        && pairs->getChildElement(0)->getStringAttribute("slot") == "fixture-track-uuid";
                }
            }
            if (app) metadataOk = xml.getBoolAttribute("follow")
                && xml.getStringAttribute("app") == (scenario == 5 ? "*friend:142*" : "*link*");
            check(metadataOk, "requested metadata survives the released serializer");
        }
        juce::MemoryBlock state;
        processor->getStateInformation(state);
        const auto filename = juce::String(scenario) + "-" + labels[scenario];
        const auto stateFile = output.getChildFile(filename + ".bin");
        if (!stateFile.replaceWithData(state.getData(), state.getSize())
            || !output.getChildFile(filename + ".xml").replaceWithText(xml.toString())) return 1;

        juce::Array<juce::var> parameters;
        std::vector<float> expected;
        for (auto* parameter : processor->getParameters()) {
            auto* item = new juce::DynamicObject();
            item->setProperty("index", parameter->getParameterIndex());
            item->setProperty("name", parameter->getName(256));
            item->setProperty("label", parameter->getLabel());
            item->setProperty("normalizedValue", parameter->getValue());
            item->setProperty("defaultNormalizedValue", parameter->getDefaultValue());
            item->setProperty("numSteps", parameter->getNumSteps());
            item->setProperty("discrete", parameter->isDiscrete());
            item->setProperty("automatable", parameter->isAutomatable());
            item->setProperty("meta", parameter->isMetaParameter());
            item->setProperty("minimumText", parameter->getText(0.0f, 256));
            item->setProperty("maximumText", parameter->getText(1.0f, 256));
            item->setProperty("defaultText", parameter->getText(parameter->getDefaultValue(), 256));
            item->setProperty("currentText", parameter->getCurrentValueAsText());
            parameters.add(juce::var(item));
            expected.push_back(parameter->getValue());
            if (scenario == 1) check(std::abs(parameter->getValue() - parameter->getDefaultValue()) > 0.00001f,
                "parameter changed from its default");
        }
        processor.reset();   // A live instance with this UUID would make the restored Track reclaim its identity.
        auto restored = load(fm, bundlePath(path));
        if (!restored) return 1;
        juce::MemoryBlock disk;
        if (!stateFile.loadFileAsData(disk)) return 1;
        restored->setStateInformation(disk.getData(), int(disk.getSize()));
        HubState roundTrip(*restored);
        check(roundTrip.plugin && xml.isEquivalentTo(roundTrip.plugin.get(), false), "saved state restores every XML property and child in a fresh instance");
        bool parametersOk = restored->getParameters().size() == int(expected.size());
        for (int index = 0; parametersOk && index < int(expected.size()); ++index) {
            const auto actual = restored->getParameters()[index]->getValue();
            parametersOk = std::abs(actual - expected[size_t(index)]) < 0.00001f;
            if (!parametersOk) std::fprintf(stderr, "%s parameter %d: expected %.9g, restored %.9g\n",
                labels[scenario].toRawUTF8(), index, expected[size_t(index)], actual);
        }
        check(parametersOk, "saved state restores every host parameter in a fresh instance");
        auto* entry = new juce::DynamicObject();
        entry->setProperty("label", labels[scenario]);
        entry->setProperty("stateFile", stateFile.getFileName());
        entry->setProperty("componentXmlFile", filename + ".xml");
        entry->setProperty("stateBytes", juce::int64(state.getSize()));
        entry->setProperty("parameters", juce::var(parameters));
        cases.add(juce::var(entry));
        processor = std::move(restored);
    }
    manifest->setProperty("cases", juce::var(cases));
    manifest->setProperty("roundTripPassed", failures == 0);
    if (!output.getChildFile("manifest.json").replaceWithText(juce::JSON::toString(manifestVar, false))) return 1;
    return failures == 0 ? 0 : 1;
}
