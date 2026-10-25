from conan import ConanFile
from conan.tools.cmake import cmake_layout, CMakeToolchain, CMakeDeps

class BglEngineRecipe(ConanFile):
    name = "bgl_engine"
    version = "0.9.11"
    package_type = "application"
    settings = "os", "arch", "compiler", "build_type"

    options = {
        "with_audio": [True, False],
        "with_testing": [True, False],
        "with_png": [True, False],
        "with_jpeg": [True, False],
        "with_webp": [True, False],
        "with_ktx": [True, False]
    }
    default_options = {
        "with_audio": True,
        "with_testing": True,
        "with_png": True,
        "with_jpeg": True,
        "with_webp": True,
        "with_ktx": True,
        "spdlog/*:header_only": True,
        "harfbuzz/*:with_glib": False
    }

    def requirements(self):
        self.requires("fastgltf/0.7.2")
        self.requires("glm/1.0.1")
        self.requires("spdlog/1.14.0")
        if self.options.with_png:
            self.requires("libpng/1.6.43")
        if self.options.with_jpeg:
            self.requires("libjpeg-turbo/3.0.2")
        if self.options.with_webp:
            self.requires("libwebp/1.3.2")
        if self.options.with_ktx:
            self.requires("ktx/4.3.2")
        if self.options.with_audio:
            self.requires("miniaudio/0.11.21")
        self.requires("freetype/2.13.2")
        self.requires("harfbuzz/8.3.0")
        if self.options.with_testing:
            self.requires("gtest/1.14.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.variables["CMAKE_CXX_STANDARD"] = "23"
        tc.variables["CMAKE_CXX_STANDARD_REQUIRED"] = "ON"
        tc.variables["CMAKE_CXX_EXTENSIONS"] = "OFF"
        tc.variables["BGL_ENABLE_PNG"] = self.options.with_png
        tc.variables["BGL_ENABLE_JPEG"] = self.options.with_jpeg
        tc.variables["BGL_ENABLE_WEBP"] = self.options.with_webp
        tc.variables["BGL_ENABLE_KTX"] = self.options.with_ktx
        tc.user_presets_path = "ConanPresets.json"
        tc.generate()

