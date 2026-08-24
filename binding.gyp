{
  "targets": [
    {
      "target_name": "pi_tinycc",
      "sources": [
        "native/pi_extension_node.c",
        "native/pi_typed_node.c",
        "native/pi_tinycc_node.c"
      ],
      "include_dirs": [
        "include",
        ".deps/tinycc/prefix/include"
      ],
      "defines": [
        "_GNU_SOURCE",
        "NAPI_VERSION=8",
        "PI_TCC_LIB_PATH=\"<(module_root_dir)/.deps/tinycc/prefix/lib/tcc\""
      ],
      "cflags": [
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fvisibility=default"
      ],
      "xcode_settings": {
        "GCC_C_LANGUAGE_STANDARD": "c11",
        "GCC_SYMBOLS_PRIVATE_EXTERN": "NO",
        "WARNING_CFLAGS": ["-Wall", "-Wextra", "-Werror"]
      },
      "libraries": [
        "<(module_root_dir)/.deps/tinycc/prefix/lib/libtcc.a"
      ],
      "conditions": [
        ["OS=='linux'", {
          "libraries": ["-lm", "-ldl", "-lpthread"]
        }]
      ]
    }
  ]
}
