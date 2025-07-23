Import("env")

idf_flags = [
    "-Wno-error=switch",
    "-Wno-error=unused-parameter",
    "-Wno-error=return-type"
]

# Only add these if ESP-IDF is used
if "espidf" in env.get("PIOFRAMEWORK", []):
    print("Applying extra CXXFLAGS for ESP-IDF framework")
    env.Append(CXXFLAGS=idf_flags)
