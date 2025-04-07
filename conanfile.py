from conan import ConanFile
from conan.tools.files import copy
from conan.tools.cmake import CMakeToolchain
from os import path


class App(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps"
    default_options = {
        "openssl*:shared": True,
        "boost*:header_only": True,
        "with_boost": True,
        "with_openssl": False,
    }
    options = {
        "with_boost": [True, False],
        "with_openssl": [True, False],
    }

    def requirements(self):
        if self.options.get_safe("with_boost", True):
            self.requires("boost/1.87.0")
        if self.options.get_safe("with_openssl", False):
            self.requires("openssl/3.3.2")

    def generate(self):
        tc = CMakeToolchain(self)
        tc.user_presets_path = False
        tc.generate()

        def copy_bin(dep, selector, subdir):
            src = path.realpath(dep.cpp_info.bindirs[0])
            dst = path.realpath(path.join(self.build_folder, subdir))

            if src == dst:
                return

            copy(self, selector, src, dst, keep_path=False)

        for dep in self.dependencies.values():
            # macOS
            copy_bin(dep, "*.dylib", ".")
            # Windows
            copy_bin(dep, "*.dll", ".")
            # Linux
            copy(
                self,
                "*.so*",
                dep.cpp_info.libdirs[0],
                path.join(self.build_folder, "."),
                keep_path=False,
            )
