# Security provenance

What has been ported from ET: Legacy (ETL) and the Quake III port (Q3A), what proves
it, and what has not been done. A cross-build proves only that code compiles.
"Reviewed" means an independent reviewer read it.

## Done

| Family | Source | Evidence |
| --- | --- | --- |
| Truncated or hostile bit reads and writes (`MSG_ReadBits`, `MSG_WriteBits`, Huffman) | ETL `bb2c0e3e`, ioquake3 `d2b1d124`, `1e309787` (including its partial-bits `>=`, which ETL lacks) | `tests/security/test_msg.c` under ASan and UBSan. Fails on the original tree at a misaligned load in `msg.c` |
| Unaligned OOB integers | Q3A | Same test. OOB fields are assembled from bytes |
| Delta field counts past the table, or -1 at end of message | ETL `msg.c` | `test_bad_field_counts` |
| Fragment length, reassembly into `maxsize - 4` | Q3A `7681d890` | Code review; no dedicated test. Its larger receive buffer is not taken: ET: Legacy's client receives into exactly `MAX_MSGLEN` too. `FRAGMENT_BIT` is unsigned, as there, with the same bit on the wire |
| Oversized packet leaking its zone block in `Com_EventLoop` | Q3A (`break` for `continue`) | `tests/mac/run_mac_net_tests.sh`: an oversized packet is dropped and every event block freed once, under ASan's leak check |
| A quoted `+` splitting a startup command | Quake III 1.32 as released; ET 2.60 computes the quote state and never uses it | `tests/mac/run_mac_startup_parms_tests.sh` |
| Gamestate `clientNum` range, snapshot area mask length | Q3A #40, ioquake3 `99abd01c` | Code review; no dedicated test |
| Format strings with network text | ETL `62ff3687`, `58a49288`; ioquake3 `59c231c6` | The Mac build fails on any such call (`-Werror=format-security`). 16 sites fixed |
| Client file overwrite through server pak names, and path traversal including HFS `::` | CVE-2006-3324 (ioquake3 `813a6ecd`); Q3A #262 | `tests/security/test_path.c`, with `/` and `:` separators |
| Spoofed `print` dropping a client, spoofed `challengeResponse`, reflected `echo` | Local; stricter than ETL | `SPOOF=30 tests/compat/run.sh`: an `[err_dialog]` packet from another port drops the retail client (`ERROR: spoofed`) and not this one |
| Auto-update, which could start a process from a server's say-so | Local | Off in static builds. Code review |
| Server overwriting registered client cvars through systeminfo | ETL `cl_parse.c` | Code review, and a real server still connects. No hostile-server test. A server can still seed a cvar no module has registered yet, and the value survives registration, as in ETL and ioquake3 |
| Server lists accepted from any host; `getservers` sent to an address never filled in for any master but the first, or one that did not resolve | Q3A `3f7881d7` (its store bounds were already here) and the master half of `cb934e88`. The legacy menus ask every `sv_masterN` that is set, so each is resolved and recorded, and only those addresses may answer | `tests/compat/master.sh`: two fake masters on loopback fill the list, a server both list once, while 50 entries from an unasked port are refused; with the check disabled it fails. Before, neither master was asked. Its length checks are taken too, and a server two masters list is stored once |
| Amplification through `getstatus`, `getinfo`, `getchallenge` | CVE-2010-5077; simpler than Q3A's buckets. rcon keeps its existing global limit | Code review |
| `FS_VerifyOfficialPaks`: stack overflow from repeated names, NULL from a short name list | Local review; ETL has the same code | `tests/security/test_official.c` extracts the function and runs it under ASan |
| BMP, PCX, TGA and JPEG loaders: reads past the file, size overflow, RLE and palette writes | Q3A (`731b6de3`, `ad93870f`, `c1373bb4`, `13b74609`, with its retail tolerance follow-ups `570315ad`, `e88683af`); JPEG decode written for this tree | `tests/security/test_images.c`: truncation at every length and 320,000 mutated files under ASan and UBSan. All 1,666 images in pak0-2 and the `legacy` pk3 (1,208 TGA, 458 JPEG) decoded to the same size and pixels as the old loaders, by a one-off comparison kept in `~/et-data/evidence/image-loaders`. The paks hold no BMP, PCX or grayscale JPEG, so those have only synthetic files. BMP also now honours top-down rows and row padding, which id ignored |
| Grayscale JPEG heap overflow: id filled one byte a pixel, then wrote alpha through four | Local, found reading the code. RGB_PIXELSIZE is 4 in this libjpeg, unlike Q3A's | Same test, `gray jpg` |
| libjpeg: scan names a component twice | CVE-2013-6629, from libjpeg-turbo. Found by the mutation test as a NULL row store | `duplicate jpeg component` |
| libjpeg: a scan that lists its components in another order wrote one plane's rows into another, past its end, with file-chosen data | Local. Found by the independent review, after the mutation test missed it | `reordered jpeg scan`; the review then ran 1.06M structured mutations clean under ASan and UBSan |
| `R_FindImageFile`: stack overflow from a long `map` token in a shader | Local, found by the review | Code review. Needs a whole renderer to test |
| `Upload32`: a 4096x1 texture hung the client when the GL limit halved its height to 0 | Local, found by the review. Sides are now capped at 4096 too | Code review for the clamp; `long thin tga` for the cap |
| libjpeg: Huffman table's stack garbage copied out | CVE-2013-6630, from libjpeg-turbo | Code review |
| libjpeg: DC symbol past `extend_test`, codes too long for the lookahead table | libjpeg 6b. The first was found by the mutation test | `bad DC symbol` |
| libjpeg error exits were `ERR_FATAL`, so one bad file quit the game | Q3A `a1fd4795` | Mutation test: a bad file loads as no image |
| WAV chunk and format bounds, sub-byte widths, rates and lengths the resamplers overflow, ADPCM temp buffer, sounds that outgrow the pool | Q3A `effec0fe`, `29c2d6bd`, `cb2dd0a3`, `80753e78`, `111f7aa2`, part of `dcc71126` and `716f7778` | `tests/security/test_wav.c` tests `GetWavinfo` under ASan and UBSan with truncation and 200,000 mutated headers. All 1,166 WAVs in the paks report the same format, length and offset as before. The review found a 0-sample 16-bit stream swapped its way up the stack on big-endian, now refused, with the swap tested. The resampler, pool and mixer guards have code review only |
| Evicting a sound that is still playing | Not ported from Q3A (`dcc71126`). The mixer now skips a sound with no data, so this costs a dropped sound, not a NULL read | Code review |
| pk3 zip reader: unterminated names, false entry counts, short reads taken as data, truncated stored entries, read position after metadata calls, decoder replacement | Q3A `2e59e544`, `40912889`, `2bd8a321`, `4c73c900`, `c73bd683`, `4d648eb6`, `b65457cb`, `ed0bff82`, `579bd4da`, and its `FS_LoadZipFile` | `tests/security/test_zip.c` under ASan and UBSan: crafted long name, false entry count and short stored entry, truncation, and 60,000 mutated files through the reader and `FS_LoadZipFile`. The host client joins with `sv_pure 1`, so the real paks still checksum the same |
| zlib 1.1.3 double free of the bit length table | CVE-2002-0059, as zlib 1.1.4 | `test_zip.c`, with one crafted block: this zlib answers `Z_MEM_ERROR` from the data alone when a table outgrows `MANY` |
| An entry declaring 2 GB or more reached callers as a negative length, and `FS_ReadFile` wrote the whole payload past its buffer. One just under 2 GB wrapped the hunk's own check to the same end, so entries over 1 GiB are refused and the check is made before the arithmetic. A short read returned stale hunk memory. A reopened handle shared the pak handle's decoder, for a use-after-free after an `ERR_DROP` | Q3A `204fe36d`, part of `03ec911e` and `d5ccf9d4`, and the `files.c` half of `40912889`. The 1 GiB cap and hunk check are local, and Q3A's hunk has the same wrap. All found by the independent review | The reviewer reproduced each under ASan with the real `files.c` and hunk code, the size bugs at `-m32` too, and confirmed the refusals at both word sizes. That harness is not in `tests/`. A real read of every entry in the four pk3s had no failures |
| `SV_WriteDownloadToClient` checks | CVE-2006-2082 | Already in the 2.60 GPL source |
| Unchecked allocations a Mac reaches (`R_GetImageBuffer`, `R_Hunk_Begin`, `SND_setup`) | Local | The first was found on the emulator (commit aa88b4d) |
| Shader with more stages than `MAX_SHADER_STAGES` (8) wrote past the stage table | Q3A `4387c9a2`. Its tail recovery (`099bb702`) is not taken: nothing resumes from a refused parse here | `tests/security/test_shader.c`, case `stages`: ASan overflow in `ParseStage` on the old tree; 9 to 12 stages now make `ParseShader` fail, so `R_FindShader` marks the shader `defaultShader` as for any parse error. Callers that ignore that flag (skins, models) keep its first 8 stages, as before and as in Q3A |
| `tcMod` line joined into a 1024-byte stack buffer by `strcat` | Q3A `ebc90ef7` (it refuses the line; here it is cut at 1023 characters) | Case `tcmod`, a 3,600-character line: ASan stack overflow on the old tree |
| Lightmap index at or past `MAX_LIGHTMAPS` stored an external `lm_` image past `tr.lightmaps`; an unknown negative index read before it; `sprintf` of the world path | Local for the upper bound (ETL has the same bug); Q3A `f7f70413` for negatives. Both are now vertex lit, and the four special values are kept | Cases `lightmap-high`, `lightmap-low`: UBSan index 256 and -5 on the old tree; `lightmap` passes on both and guards the valid indexes |
| NULL name to `RE_RegisterShader`, `RE_RegisterShaderNoMip`, `RE_RegisterShaderLightMap`; `tolower` of a signed char in both name hashes | Q3A `f7f70413` | Case `null`: UBSan null `strlen` argument on the old tree. Retro68's newlib has no negative ctype indexes, so on the Mac a byte of 0x80 or more read before `_ctype_`; the hashes are now deterministic there, and unchanged on the host. The same cast is applied to the upper-case extension retry in `R_FindImageFile` |
| Skins: more than 32 surfaces or 5 `md3_` models, a quoted token's terminator one past its buffer in `CommaParse`, and a pointer-sized default skin surface | Q3A `f9aa0602` (it refuses such a skin; here the extra lines are skipped with a warning) | `tests/security/test_skin.c`: each fails under ASan or UBSan on the old tree. All 2,779 shaders in the paks' `scripts/` and 186 skins parse to the same results as before (7 malformed retail shaders refused by both; at most 7 stages, 22 surfaces, 3 models), by a one-off comparison in `~/et-data/evidence/shader-skin` |
| `COM_ParseExt`: a quoted token of 1024 characters or more wrote its NUL at `com_token[1024]` | ETL `22c68029` (two of the four bounds); the other two local. The plain-word path was already safe: a word reaching 1024 is discarded. `src/splines` has its own copy, bounded in length; its unterminated quote is fixed below. `legacy`'s is the mod's | `tests/security/test_text.c` `quoted`: 1023, 1024, 1025 and 4000 characters, plain and string-in-string, real `q_shared.c` under ASan and UBSan. Fails on master (`index 1024 out of bounds`). All 683 `.shader`, `.menu`, `.cfg` and `.skin` files in the four pk3s give the same 503,138 tokens before and after |
| `COM_ParseExt`: an unterminated quote left the cursor one past the NUL, so the next call read past the buffer | Local (Q3A `e4be26dc` returns NULL instead). Retail `pak0.pk3:lloydm.cfg` ends inside a quote | `test_text.c` `unterminated`; fails on master with a heap read past the buffer in `SkipWhitespace` |
| `Info_SetValueForKey(_Big)`: a pair making the string exactly the buffer size wrote its NUL past it. `cl_main.c` appends `nettype` to a server's info this way | Q3A, ETL (`>=`) | `test_text.c` `info`, at size - 1, size and size + 1 for both; fails on master in `strcat` |
| `FindMatches`: the cut of `shortestMatch[1024]` ran the length of the candidate, and a server's systeminfo can create cvar names up to 8191 bytes | ioquake3; Q3A `7ef6c86b`, whose test is adapted. No other completion helper has the pattern | `test_text.c` `complete` extracts the statics and `FindMatches` from `common.c`; fails on master (`index 1024 out of bounds`) |
| `FS_ListFilteredFiles`: read `path[-1]` for an empty path | ETL `13f30fcb` | `tests/security/test_fs_text.c` `list` extracts it from `files.c`; fails on master with a heap read before the path |
| Key files by name: the read guard's `strstr( "etkey" )` was case-sensitive and HFS is not, and the existence probe and write, append and rename paths had no guard. `rtcwkey`, the CD key file, had none at all | Adapted from Q3A `09cf4690` (`q3key`, a substring match); the whole-last-component match and `rtcwkey` are local. Defence in depth here: `legacy` is native code, not a VM, so it could open the file without the engine | `test_fs_text.c` `keyread` and `keywrite`: `ETKEY`, `EtKey`, path prefixes and `rtcwkey` refused by the real read guard, probe, `FS_FOpenFileWrite`, `FS_FOpenFileAppend` and `FS_Rename`, ordinary names still pass. Fails on master. The engine still writes both through `FS_SV_*` and reads `etkey` at start-up |
| The same cursor fault in `SkipRestOfLine` (shader lines such as `qer_editorimage` at the end of a dynamic shader) and the splines' own `Com_ParseExt` (a `cameras/*.camera` file a server's `startCam` names) | ioquake3 for `SkipRestOfLine`; local for splines | `test_text.c` `unterminated`, real `q_shared.c`. The splines copy has code review only |
| The GUID was hashed from `etkey` through `FS_ReadFile`, which searches pk3s before the folder, so a downloaded pk3 holding an `etkey` entry chose `cl_guid`; only the size check read the real file | Local, found by the review | Code review: `Com_MD5FileETCompat` now reads `etmain/etkey` through `FS_SV_FOpenFileRead`, as the size check does. Same bytes hashed, so the GUID is unchanged |
| `CL_ParseDownload` www redirect: the server's URL became `downloadName`, so UDP blocks sent after the redirect were renamed to any path under, or with `..` above, `fs_homepath` | Local; ETL has the same flow | `test_clparse redirect`: master renames the blocks to the URL (`../../../.bashrc`); now the names stay, blocks are ignored until the new gamestate, and the UDP fallback still completes. The EOF rename and `CL_WWWDownload` re-check `FS_CheckDownloadName` (`eof_rename_checked`) |
| Download names longer than the name buffers, and HFS's 31-byte limit for `<pak>.tmp` | Q3A `204fe36d` (lengths), `300f7323` (temp name) | `test_clparse begin_download_names` with `/` and `:`. Retail, legacy and 17 map pak names still download |
| Binary bytes after `svc_EOF` before the cgame loads: `VM_Call( NULL )` is `ERR_FATAL` | ETL `8c47c2e0a`, which drops; here they are ignored | `test_clparse binary_without_cgame` |
| One snapshot with more entities than the parse ring | Q3A `53440e7f` (CVE-2018-12100 family) | `test_clparse entity_count`: master parses 2056 into a 2048 ring; now `ERR_DROP`, and a full frame of 1023 still parses |
| Cvars a server creates: flags 0, so no later server could set them, and the player could | ETL and ioquake3 `CVAR_SERVER_CREATED`; Q3A `18b0dbd1` (registration drops its `CVAR_ROM`) | `test_clparse systeminfo_cvars` on the real `cvar.c`. Modules cannot claim the flag |

## Not done

- ETL's `Com_Printf` overflow fix (`c10805a4`) and `Q_ColorizeString` (`89647300`) are not
  reviewed against this tree yet.
- `sv_userInfoFloodProtect` (`f5258c7d`) and the other server-side ETL fixes matter only for a
  listen server and are not ported.
- BSP, model and cinematic loaders take pk3 content from a server, and are not hardened
  yet. Shader and skin text has its array bounds only: Q3A's checks of non-finite numbers,
  fallback state and its brace recovery are not ported. `inflate` itself is zlib 1.1.3 with the fix above and has only had the mutation test. Q3A has a long validation series (`docs/review-*`
  there). **A malicious server's pk3 remains the largest open risk**, and `sv_pure` does not
  defend a client against it.
- The botlib preprocessor, which also reads the menus from pk3s, merges tokens with `##` in
  `PC_MergeTokens` by an unbounded `strcat` into a 1024-byte token, for two strings and for a
  name with a name or number. Q3A bounded that in `08460fb3`. Its `fa39407b` and `f9f73568`
  bound includes, expressions, live tokens and nested conditionals, which ET has not reviewed.
- `cm_optimizePatchPlanes` is not followed (E5). When it is, read it from `systemInfo`, as
  ETL does, rather than from the cvar.

- No sanitizer run of the whole engine. Only `msg.c`, `huffman.c` and `fs_path.c` have one.

## Decided, and stricter than upstream

`challengeResponse` is taken only from the IP the client asked. ET: Legacy takes it
from anyone, so a proxy can hand a connection to another address. Here that is refused,
because a forged one moves the connection to the sender. A multi-homed server that answers
from another address without `net_ip` set will not connect, but most home NATs would drop
that reply too.

## Quake III engine commits since the copy point

Quake III's non-Mac commits after `72dfd69f`, the point `src/mac9` was first copied from, checked
against this tree on 2026-11-08 (`git log --no-merges 72dfd69f..HEAD -- code ':!code/mac'`). The Mac layer itself is synced separately (to `496b52ee`).

| Q3A | Here |
| --- | --- |
| `bd45aa07`, `a5580ba9` CVE-2002-0059, `blens` freed once | Already here, from zlib 1.1.4 (`80830f8`, before Q3A's). `tl` and `td` are not set to NULL before `inflate_trees_dynamic` as in Q3A, and need not be: they are read only after it returns `Z_OK`, which sets both |
| `e0368a99` temporary hunk size checked before the arithmetic | Already here (`1167cf6`, before Q3A's) |
| `dc614e10` JPEG scan components to their own planes, CVE-2013-6629 | Already here (`jdcoefct.c:190`, `jdmarker.c:311`), the rows for the reordered scan and the duplicate component above |
| `3eff444f` old gamestates rewriting a listen server's cvars | Already here (`4bf204e`) |
| `1f6cc686` no `disconnect` queued into a full command queue | Already here (`4bf204e`) |
| `65ef8292` no disconnect packets with a wrapped queue | Ported (PR #16) |
| `83514210` ADPCM painting wraps at the end of a sound | The wrap is already here: id's paint loop had it, and `fc15827` added it to the first walk along the chain. Q3A also resets the scratch chunk index on the wrap; here the index keeps counting, which costs at most a repeated decode, since an index still names one chunk |
| `241e767d` no print for an empty shader name | Does not apply: ET never added the check that printed |
| `cb934e88`, `7582058f` id's update and authorize hosts, `com_authServer` | Nothing is asked of id's or Activision's hosts. `AUTHORIZE_SUPPORT` is not defined anywhere, so no CD key is sent and a challenge is answered at once; there is no `com_authServer` to protect. With `ET_STATIC_MODULES` (the Mac and host builds) `cl_motd` is 0, so its `etmaster.idsoftware.com` is never looked up, the auto-update checks return at once, and the master is `master.etlegacy.com`. Ported in PR #17 (`58d4073`, from Q3A `3f7881d7` and these): every `sv_masterN` resolved, only masters asked may answer. Ported in PR #19: a port in `sv_masterN` kept for heartbeats, whose `strstr` arguments were reversed |
| `70d2a8c4` `teamleader` sized as a name | Does not apply to the Mac build: Quake III game code. ET's `src/botai` has the same 32-byte field and writes its terminator one past it (`ai_cmd.c:1037`, fixed in Q3A by `18aa471f`), but neither ET's own game module nor its bots are built, and `legacy` has neither |
| `05f2071c` bots hanging the server on a spawn point | Does not apply: Quake III game code. `legacy`'s only bots, Omni-bot, are not built (`FEATURE_OMNIBOT` is in no `legacy/static/*.defines`), and its spawn selection has no retry loop |
| `efab18d9` `AAS_AreaVolume`, `9a39aa4c`, `756caaa4` optimized AAS files | Do not apply: botlib is linked, but an AAS file is loaded only through `BOTLIB_LOAD_MAP` (`sv_game.c:566`), and `legacy` has no such trap (`legacy/src/game/g_public.h` keeps the setup traps only as dummies), so no AAS file is read. Only botlib's script preprocessor runs, for the menus (see Not done) |
| `18aa471f` `-O2` by default | ET's own (PR #14, `--bringup` for `-O0` in PR #15). Its two code changes: the `teamleader` write is the `70d2a8c4` row. It aligns `tess` for R_Init's `tess.xyz` warning; here `tess.xyz` points at the separate `tess_xyz` array, so that would not help, and the warning is harmless: no SIMD code reads it |
