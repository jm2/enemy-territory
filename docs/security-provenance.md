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
| Server overwriting registered client cvars through systeminfo | ETL `cl_parse.c` | Code review, and a real server still connects. No hostile-server test. A server can still seed a cvar not yet registered, which ETL closes with `CVAR_SERVER_CREATED` |
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

## Not done

- ETL's `Com_Printf` overflow fix (`c10805a4`), `Q_ColorizeString` (`89647300`), autocomplete
  (`13f30fcb`) and `COM_ParseExt` (`22c68029`) are not reviewed against this tree yet.
- `sv_userInfoFloodProtect` (`f5258c7d`) and the other server-side ETL fixes matter only for a
  listen server and are not ported.
- BSP, model, shader and cinematic loaders take pk3 content from a server, and are not hardened
  yet. `inflate` itself is zlib 1.1.3 with the fix above and has only had the mutation test. Q3A has a long validation series (`docs/review-*`
  there). **A malicious server's pk3 remains the largest open risk**, and `sv_pure` does not
  defend a client against it.
- The botlib preprocessor, which also reads the menus from pk3s, merges tokens with `##` in
  `PC_MergeTokens` by an unbounded `strcat` into a 1024-byte token, for two strings and for a
  name with a name or number. Q3A bounded that in `08460fb3`. Its `fa39407b` and `f9f73568`
  bound includes, expressions, live tokens and nested conditionals, which ET has not reviewed.
- `cm_optimizePatchPlanes` is not followed (E5). When it is, read it from `systemInfo`: the
  cvar keeps the first server's value.

- No sanitizer run of the whole engine. Only `msg.c`, `huffman.c` and `fs_path.c` have one.

## Decided, and stricter than upstream

`challengeResponse` is taken only from the IP the client asked. ET: Legacy takes it
from anyone, so a proxy can hand a connection to another address. Here that is refused,
because a forged one moves the connection to the sender. A multi-homed server that answers
from another address without `net_ip` set will not connect, but most home NATs would drop
that reply too.
