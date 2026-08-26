# Submits the disk image to Apple, waits for the answer, and staples the ticket.
#
#     cmake -D DMG=... -P Notarize.cmake
#
# The credentials come from the environment and only from the environment:
#
#     XPCOG_NOTARY_KEY        path to the App Store Connect .p8 private key
#     XPCOG_NOTARY_KEY_ID     the key's ten-character id
#     XPCOG_NOTARY_ISSUER_ID  the issuer UUID for the team the key belongs to
#
# No cache variable and no -D, which is a departure from every other setting in
# this project and is the point. A -D lands in CMakeCache.txt and in build.ninja,
# both of which get committed by accident, copied into a bug report, and left in
# a build tree that outlives the reason it was configured. The .p8 is a private
# key that can notarise anything under the team's name for as long as it lives,
# and the two ids are what make it usable. Nothing here writes any of them
# anywhere.
#
# An App Store Connect key rather than an Apple ID and an app-specific password,
# which notarytool also accepts. The password form ties releases to one person's
# Apple ID, which then cannot have two-factor re-enrolled without breaking CI,
# and it is a credential that unlocks rather more than notarisation. A notary key
# does one thing and can be revoked in the App Store Connect UI without anyone
# losing access to anything else.
#
# --- What stapling is for --------------------------------------------------
#
# Notarisation puts a ticket on Apple's servers. Without stapling, the first Mac
# to open the image asks for it over the network -- which works, until the
# machine is offline or Apple is having a day, and then Gatekeeper fails closed
# on a download that is perfectly good. Stapling attaches the ticket to the file,
# so the check is local and the answer is the same everywhere.
# ---------------------------------------------------------------------------

if(NOT DMG OR NOT EXISTS "${DMG}")
    message(FATAL_ERROR
        "Notarize: no disk image at '${DMG}'. Build the `dmg` target first.")
endif()

set(_missing "")
foreach(_var XPCOG_NOTARY_KEY XPCOG_NOTARY_KEY_ID XPCOG_NOTARY_ISSUER_ID)
    if(NOT DEFINED ENV{${_var}} OR "$ENV{${_var}}" STREQUAL "")
        list(APPEND _missing ${_var})
    endif()
endforeach()

if(_missing)
    string(REPLACE ";" ", " _missing_text "${_missing}")
    message(FATAL_ERROR
        "XPCog: cannot notarise -- ${_missing_text} not set in the environment.\n"
        "Create an App Store Connect API key with the Developer role at\n"
        "  https://appstoreconnect.apple.com/access/integrations/api\n"
        "then point XPCOG_NOTARY_KEY at the downloaded .p8, XPCOG_NOTARY_KEY_ID "
        "at its key id and XPCOG_NOTARY_ISSUER_ID at the issuer UUID shown above "
        "the key list.")
endif()

set(_key "$ENV{XPCOG_NOTARY_KEY}")
set(_key_id "$ENV{XPCOG_NOTARY_KEY_ID}")
set(_issuer "$ENV{XPCOG_NOTARY_ISSUER_ID}")

if(NOT EXISTS "${_key}")
    message(FATAL_ERROR "XPCog: XPCOG_NOTARY_KEY points at no file: ${_key}")
endif()

# The image *must* already be signed with a Developer ID and the hardened
# runtime, or Apple rejects it -- several minutes from now, in a log file, with a
# message about the wrong thing. Asked here instead, where the answer is instant.
execute_process(
    COMMAND codesign --verify --strict "${DMG}"
    RESULT_VARIABLE _unsigned
    ERROR_QUIET OUTPUT_QUIET)
if(NOT _unsigned EQUAL 0)
    message(FATAL_ERROR
        "XPCog: ${DMG} is not signed. Apple notarises Developer ID signed code "
        "and nothing else. Set XPCOG_CODESIGN_IDENTITY and rebuild the `dmg` "
        "target.")
endif()

message(STATUS "XPCog: submitting ${DMG} to the notary service; this takes minutes.")

# --wait blocks until Apple has an answer, so the exit status means something.
# Without it the command succeeds the moment the upload finishes, which says
# nothing at all about whether the image was accepted.
execute_process(
    COMMAND xcrun notarytool submit "${DMG}"
            --key "${_key}" --key-id "${_key_id}" --issuer "${_issuer}"
            --wait --timeout 30m --output-format json
    OUTPUT_VARIABLE _json
    ERROR_VARIABLE _stderr
    RESULT_VARIABLE _rc)

string(REGEX MATCH "\"id\"[ \t]*:[ \t]*\"([^\"]+)\"" _ "${_json}")
set(_id "${CMAKE_MATCH_1}")
string(REGEX MATCH "\"status\"[ \t]*:[ \t]*\"([^\"]+)\"" _ "${_json}")
set(_status "${CMAKE_MATCH_1}")

if(NOT _status STREQUAL "Accepted")
    if(_id)
        # The log, and this is the whole reason this is a script rather than a
        # command line. `notarytool submit` reports "Invalid" and stops; the
        # *reasons* -- an unsigned nested binary, a missing hardened runtime
        # flag, a secure timestamp that was not there -- are in a log fetched by
        # submission id, and a release engineer who has to be told to go and
        # fetch it is being told twenty minutes later than they could have been.
        message(STATUS "XPCog: notarisation returned '${_status}'. Apple's log:")
        execute_process(
            COMMAND xcrun notarytool log "${_id}"
                    --key "${_key}" --key-id "${_key_id}" --issuer "${_issuer}")
        message(FATAL_ERROR
            "XPCog: notarisation failed (status '${_status}', submission ${_id}).")
    endif()

    # No submission id means the upload never happened -- bad credentials, an
    # unreadable key, no network -- so there is no log to fetch and saying
    # "Apple's log:" above a silence is worse than saying nothing. What there is
    # instead is whatever notarytool put on stderr, which in that case is the
    # entire diagnosis and belongs at the top of the message rather than under a
    # status line that has nothing in it.
    message(FATAL_ERROR
        "XPCog: the notary service accepted no submission.\n"
        "${_stderr}\n"
        "invalidPrivateKeyContents means XPCOG_NOTARY_KEY is not a readable "
        "App Store Connect .p8; check that the file begins with "
        "-----BEGIN PRIVATE KEY-----.")
endif()

execute_process(
    COMMAND xcrun stapler staple "${DMG}"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(
    COMMAND xcrun stapler validate "${DMG}"
    COMMAND_ERROR_IS_FATAL ANY)

# The last check, and the only one that asks the question the user's Mac will
# ask: not "is this signed" or "was this notarised" but "would Gatekeeper open
# it". context:primary-signature is the assessment a downloaded disk image gets.
execute_process(
    COMMAND spctl --assess --type open --context context:primary-signature -vv "${DMG}"
    COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "XPCog: notarised and stapled ${DMG} (submission ${_id})")
