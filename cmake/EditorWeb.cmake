# The GUI editor's web packages (tauri-editor.md §5.1.2, §5.1.3): ES
# modules as the npm registry publishes them, each a tarball pinned by its
# version and SHA-512, fetched by FetchContent and copied whole, license
# included, into the editor's web/vendor/. There is no npm, no bundler, and
# no build step: an import map names each package's entry file, and the
# webview loads the modules as they are.
#
# One row a package: its name, its version, its ES module entry, and the
# SHA-512 of its tarball (the registry's dist.integrity, in hex). The list
# is every package the editor imports and every package they import.
set(ESPRESSO_EDITOR_WEB_PACKAGES
    "@marijn/find-cluster-break|1.0.4|src/index.js|5b2d15efe4865239c5f7f4e488cd612950cf8fb8ca5ddb8f35ba0c56d1d303c77248c5115aa7e0fc967d1364aaf09812266925ee4ed07bd14b78c4ab4ab69699"
    "@codemirror/state|6.7.6|dist/index.js|900cfe027711b4ab8892779dc53d5bab85f05efe14a3085b9075359b23ebb556ff8d9b48996b95e415f3bf9bcaea2de46000ac76266240a15043932aede956f3"
    "crelt|1.0.7|index.js|68ae816d67e17f853fc0270b1ca3c997fc5ae95915b2d45a3f2c16b4c286c2e38b73fc194f2418ba8c60bd99cdb01befeca83761305061804282072f890733b8"
    "style-mod|4.1.5|src/style-mod.js|590da594265af6c65290903e68ec298390e47db01714a1a1fffb773114c17abb85892042f865d499b073af0ddb5f26e809601bb425bd2270df06c5eb443feb02"
    "w3c-keyname|2.2.8|index.js|769a2306136c08d37b4fcd939bb936e80e86f4c2f7364843b27c3d9ff7a8c52465541078084b4843f29308b2361707f76ad6925d1858164422dd29671703ef3d"
    "@codemirror/view|6.43.15|dist/index.js|1c7b1fa6a15e1f25cbb3df37a5245edfc5da8ecd2f156650d81b97d26eca8bcc378d9e620fc72e477941cf6ff9fd7e770e17f7d920472a1eabb524c1ead29e1b"
    "@lezer/common|1.5.6|dist/index.js|359c6edeae3d724f9861b1093ee20f23214fdfb5e7781bf33c86ab04711704b0557e466a422799fd8d9ba189c6358b668e93992e700ab3f970aad983a62dfae7"
    "@lezer/highlight|1.2.5|dist/index.js|3b518c54a82d7f962ca45691ce99aa91580cb489361c6f6e3006c7eb074021d361dd35730ac721ffe7118beea5dbffd418329ee8d07d056ddcbee4797b3310d3"
    "@lezer/lr|1.4.12|dist/index.js|3a7f47538a0c3221cf3244038284e719a863806e67fcc90ddfc0a01bbeec32b68a2cbfb72416de40d8b515c09757d411ab017dfc15173f771a1f2242d5f04e4d"
    "@codemirror/streamparser|6.0.0|dist/index.js|25314f17ac6b7e3b749a77c63e0cdc2e2330626bc8c8d6a88b3a7c058ceff0513c2ab4fa181183675f74aedb7e3bb5a47b7806fcc147ca263d60635e8a6a1fe3"
    "@codemirror/language|6.13.1|dist/index.js|cdfbd4dabdd3cb29502cc8ffb5efb90a7434ad2a998bc075a130816d091a81c2d6e1773a8a669beaa37ea44261b2e34e2da26a891c815e9f548560350e156f0f"
    "@codemirror/commands|6.11.1|dist/index.js|3bfe211b7482d58c1c990d1dd9454d0ecf80b1a3567758875716d378406ea87fba6c4c4088f2b7892fc1be963aad91944402efe190f7b0881c0a646fc377a106"
    "@codemirror/search|6.7.2|dist/index.js|8146246214f6fa7ffe54667ef04cc4e56164619519626d5538a0ee748b0daa1e36b915502746ba62cb3db1d293dcc78e61fb8bd4de80640e7ba336b43b2af42c"
)

# espresso_editor_web(<vendor> <importMap>): fetches every package into
# <vendor>/<name>/, replacing what was there, and sets <importMap> to the
# "imports" entries of an import map for a page beside <vendor>.
function(espresso_editor_web vendor importMap)
    file(REMOVE_RECURSE "${vendor}")
    set(imports "")
    foreach(row IN LISTS ESPRESSO_EDITOR_WEB_PACKAGES)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 name)
        list(GET fields 1 version)
        list(GET fields 2 entry)
        list(GET fields 3 hash)
        string(REGEX REPLACE "^.*/" "" base "${name}")  # @codemirror/state: state
        string(MAKE_C_IDENTIFIER "web_${name}" id)
        string(TOLOWER "${id}" id)
        FetchContent_Declare(${id}
            URL "https://registry.npmjs.org/${name}/-/${base}-${version}.tgz"
            URL_HASH SHA512=${hash}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(${id})
        file(COPY "${${id}_SOURCE_DIR}/" DESTINATION "${vendor}/${name}")
        if(NOT EXISTS "${vendor}/${name}/${entry}")
            message(FATAL_ERROR "EditorWeb.cmake: ${name} ${version} has no ${entry}")
        endif()
        list(APPEND imports "      \"${name}\": \"./vendor/${name}/${entry}\"")
    endforeach()
    list(JOIN imports ",\n" imports)
    set(${importMap} "${imports}" PARENT_SCOPE)
endfunction()
