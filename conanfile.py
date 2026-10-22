from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeToolchain, CMakeDeps

class BglEngineRecipe(ConanFile):
    name = "bgl_engine"
    version = "0.6.1"
    package_type = "application"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "with_audio": [True, False],
        "with_testing": [True, False]
    }
    default_options = {
        "with_audio": True,
        "with_testing": True,
        "spdlog/*:header_only": True
    }

    def requirements(self):
        self.requires("fastgltf/0.7.2")
        self.requires("glm/1.0.1")
        self.requires("spdlog/1.14.0")
        self.requires("libpng/1.6.43")
        self.requires("libjpeg-turbo/3.0.2")
        self.requires("libwebp/1.3.2")
        self.requires("miniaudio/0.11.21")
        if self.options.with_testing:
            self.requires("gtest/1.14.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.user_presets_path = "ConanPresets.json"
        tc.generate()

