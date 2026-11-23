# Release checklist

1. Confirm `VERSION`, `CHANGELOG.md`, launcher version and intended `v` tag agree. Keep the qualified engine pin.
2. Run native regression suites and `python -I tests/test_release_tools.py`. Keep logs outside tracked source.
3. Build through the shared bench lock. Run `tools/make_release.py --version VERSION --kit KIT --out OUT --with-sources`.
   This exports committed source and builds the static launcher with the pinned offline kit.
4. Verify the player ZIP with `--verify-zip`, verify both SHA-256 files, and inspect licences and corresponding sources.
   No executable from the game, overlay, sound module, disc archive, save or generated game source may be shipped.
5. Extract to a separate writable player installation. Run the bundled compiler check and launcher `--self-check`
   with an invalid disc. Check English/French screens and error handling. Exercise cancel/resume with the owner's disc.
6. Prepare from an unmodified SLUS-21027 disc with the bundled tools. Test Play, copied saves, gameplay,
   native textures, meshes, movies and sound, settings and in-game Quit. Move the installation and recheck Play and preparation.
   Preserve the original saves and checkpoint; use isolated copies for automated runs.
7. Push the scoped branch, open a PR, wait for all checks, review and merge. Tag the merged version on main.
8. Wait for the version-tag release workflow; check the draft's player ZIP, checksum and toolchain source ZIP/checksum.
   Publish after the player checks pass. Download the published artifacts, verify hashes and install outside the checkout.
9. Confirm installed launcher `--version`, `source/release.ini`, game `built_from.ini` and release version agree.
   Record checked routes and remaining acceptance limits; do not claim a full story playthrough from smoke tests.
