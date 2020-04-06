from conans import ConanFile, CMake


class DrogonConan(ConanFile):
    name = "Drogon"
    version = "1.3.0"
    license = "<Put the package license here>"
    author = "<Put your name here> <And your email here>"
    url = "<Package recipe repository url here, for issues about the package>"
    description = "<Description of Drogon here>"
    topics = ("<Put some tag here>", "<here>", "<and here>")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "use_pic": [True, False],
        "libpq_batch_mode": [True, False]
    }
    default_options = {"shared": False, "use_pic": False, "libpq_batch_mode": True}
    requires = [
        "Jsoncpp/1.9.2@bitwyre/stable",
        "openssl/1.1.1j@bitwyre/stable",
        "ZLib/1.2.11@bitwyre/stable"
    ]
    generators = "cmake"
    exports_sources = "*"
    no_copy_source = True

    def requirements(self):
        if not self.settings.os == "Windows":
            self.requires("libuuid/1.0.3@bitwyre/stable")

    def _configure_cmake(self):
        cmake = CMake(self)
        cmake.definitions['BUILD_CTL'] = False
        cmake.definitions['BUILD_EXAMPLES'] = False
        cmake.definitions['BUILD_ORM'] = False
        cmake.definitions['BUILD_SHARED_LIBS'] = self.options.shared
        cmake.definitions['BUILD_TESTING'] = False
        cmake.definitions['CMAKE_POSITION_INDEPENDENT_CODE'] = self.options.use_pic
        cmake.definitions['CMAKE_PREFIX_PATH'] = self.deps_cpp_info["Jsoncpp"].rootpath
        if not self.settings.os == "Windows":
            cmake.definitions['CMAKE_PREFIX_PATH'] += ';' + \
                self.deps_cpp_info["libuuid"].rootpath
        cmake.definitions['LIBPQ_BATCH_MODE'] = True
        cmake.definitions['OPENSSL_ROOT_DIR'] = self.deps_cpp_info["openssl"].rootpath
        cmake.definitions['ZLIB_ROOT'] = self.deps_cpp_info["ZLib"].rootpath
        cmake.configure()
        return cmake

    def build(self):
        cmake = self._configure_cmake()
        cmake.build()

    def package(self):
        cmake = self._configure_cmake()
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["drogon", "trantor"]
        if self.settings.os == "Windows":
            self.cpp_info.libs += ["ws2_32", "Rpcrt4"]
        else:
            self.cpp_info.cxxflags += ["-pthread"]
            self.cpp_info.libs += ["dl", "uuid"]
