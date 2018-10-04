import qbs 1.0

Product {

    Depends { name: "cpp" }

    property string resDir: {
        if (qbs.targetOS.contains("windows"))
            return "resources";
        else if (qbs.targetOS.contains("macos"))
            return "../Resources";
        else
            return project.installPrefix + '/share/fwbuilder-' + project.version;
    }

    qbs.optimization: "fast"
    cpp.cxxLanguageVersion: "c++14"
    cpp.discardUnusedData: qbs.buildVariant == "release"
    cpp.warningLevel: "all"
    cpp.treatWarningsAsErrors: true
    cpp.includePaths: [product.sourceDirectory]
    cpp.dynamicLibraries: []

    cpp.defines: [
        '__STDC_FORMAT_MACROS',
        'GENERATION="' + project.version.slice(0,project.version.lastIndexOf('.')) + '"',
        'VERSION="' + project.version + '"',
        'PREFIX="' + project.installPrefix + '"',
        'RES_DIR="' + resDir + '"',
        'FS_SEPARATOR="/"',
        'FWBUILDER_XML_VERSION="' + project.xmlVersion + '"'
    ]

    cpp.cxxFlags: []
    cpp.linkerFlags: []

    Properties {
        condition: project.lto
        cpp.cxxFlags: outer.concat([ "-flto" ])
        cpp.linkerFlags: outer.concat([ "-flto" ])
    }

    Properties {
        condition: qbs.toolchain.contains("mingw")
        cpp.defines: outer.concat(["WIN32_LEAN_AND_MEAN"])
        cpp.dynamicLibraries: outer.concat(["pthread"])
    }

    Properties {
        condition: qbs.toolchain.contains("gcc")
        cpp.cxxFlags: outer.concat([
            "-pipe",
            "-fPIE",
            "-D_FORTIFY_SOURCE=2",
            "-fstack-protector-strong",
            "-fasynchronous-unwind-tables"
        ])
        cpp.linkerFlags: outer.concat([
            "-pie",
            "-zdefs",
            "-znow",
            "-zrelro"
        ])
    }

    Properties {
        condition: qbs.toolchain.contains("clang")
        cpp.cxxFlags: outer.concat([
            "-pipe",
            "-fPIE",
            "-D_FORTIFY_SOURCE=2",
            "-fasynchronous-unwind-tables"
        ])
        cpp.linkerFlags: outer.concat([
            "-pie"
        ])
    }

    Properties {
        condition: qbs.toolchain.contains("xcode")
        cpp.cxxFlags: outer.concat([
            "-pipe",
            "-fPIE",
            "-fstack-protector-strong"
        ])
    }

}