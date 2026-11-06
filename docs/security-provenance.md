# Security provenance

What has been ported from ET: Legacy (ETL) and the Quake III port (Q3A), what proves
it, and what has not been done. A cross-build proves only that code compiles.
"Reviewed" means an independent reviewer read it.

## Done

| Family | Source | Evidence |
| --- | --- | --- |
| Truncated or hostile bit reads and writes (`MSG_ReadBits`, `MSG_WriteBits`, Huffman) | ETL `bb2c0e3e`, ioquake3 `d2b1d124`, `1e309787` | `tests/security/test_msg.c` under ASan and UBSan. Fails on the original tree at a misaligned load in `msg.c` |
| Unaligned OOB integers | Q3A | Same test. OOB fields are assembled from bytes |
| Delta field counts past the table, or -1 at end of message | ETL `msg.c` | `test_bad_field_counts` |
| Fragment length, reassembly into `maxsize - 4` | Q3A | Code review; no dedicated test |
| Gamestate `clientNum` range, snapshot area mask length | Q3A #40, ioquake3 `99abd01c` | Code review; no dedicated test |
| Format strings with network text | ETL `62ff3687`, `58a49288`; ioquake3 `59c231c6` | The Mac build fails on any such call (`-Werror=format-security`). 16 sites fixed |
| Path traversal, including HFS `::` and pak names from servers | CVE-2006-2082 family; ioquake3 `813a6ecd`; Q3A #262 | `tests/security/test_path.c`, with `/` and `:` separators |
| Spoofed `print` dropping a client, spoofed `challengeResponse`, reflected `echo` | Local; stricter than ETL | `SPOOF=30 tests/compat/run.sh`: an `[err_dialog]` packet from another port does not drop the client |
| Auto-update, which could start a process from a server's say-so | Local | Off in static builds. Code review |
| Server setting any client cvar through systeminfo | ETL `cl_parse.c` | Code review, and a real server still connects. No hostile-server test |
| Amplification through `getstatus`, `getinfo`, `getchallenge` | CVE-2010-5077; simpler than Q3A's buckets | Code review |
| `SV_WriteDownloadToClient` checks | CVE-2006-2082 | Already in the 2.60 GPL source |
| Unchecked allocations a Mac reaches | Local | Found on the emulator; see `docs/building-mac-os9.md` |

## Not done

- ETL's `Com_Printf` overflow fix (`c10805a4`), `Q_ColorizeString` (`89647300`), autocomplete
  (`13f30fcb`) and `COM_ParseExt` (`22c68029`) are not reviewed against this tree yet.
- `sv_userInfoFloodProtect` (`f5258c7d`) and the other server-side ETL fixes matter only for a
  listen server and are not ported.
- Renderer, BSP, model, image and zip loaders take pk3 content from a server. Q3A has a long
  validation series (`docs/review-*` there). None is ported. **A malicious server's pk3
  remains the largest open risk**, and `sv_pure` does not defend a client against it.
- `MAX_RELIABLE_COMMANDS` is still 64 (E4), and `cm_optimizePatchPlanes` is not followed (E5).
- No sanitizer run of the whole engine. Only `msg.c`, `huffman.c` and `fs_path.c` have one.
