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
| Fragment length, reassembly into `maxsize - 4` | Q3A | Code review; no dedicated test |
| Gamestate `clientNum` range, snapshot area mask length | Q3A #40, ioquake3 `99abd01c` | Code review; no dedicated test |
| Format strings with network text | ETL `62ff3687`, `58a49288`; ioquake3 `59c231c6` | The Mac build fails on any such call (`-Werror=format-security`). 16 sites fixed |
| Client file overwrite through server pak names, and path traversal including HFS `::` | CVE-2006-3324 (ioquake3 `813a6ecd`); Q3A #262 | `tests/security/test_path.c`, with `/` and `:` separators |
| Spoofed `print` dropping a client, spoofed `challengeResponse`, reflected `echo` | Local; stricter than ETL | `SPOOF=30 tests/compat/run.sh`: an `[err_dialog]` packet from another port drops the retail client (`ERROR: spoofed`) and not this one |
| Auto-update, which could start a process from a server's say-so | Local | Off in static builds. Code review |
| Server overwriting registered client cvars through systeminfo | ETL `cl_parse.c` | Code review, and a real server still connects. No hostile-server test. A server can still seed a cvar not yet registered, which ETL closes with `CVAR_SERVER_CREATED` |
| Amplification through `getstatus`, `getinfo`, `getchallenge` | CVE-2010-5077; simpler than Q3A's buckets. rcon keeps its existing global limit | Code review |
| `FS_VerifyOfficialPaks`: stack overflow from repeated names, NULL from a short name list | Local review; ETL has the same code | `tests/security/test_official.c` extracts the function and runs it under ASan |
| `SV_WriteDownloadToClient` checks | CVE-2006-2082 | Already in the 2.60 GPL source |
| Unchecked allocations a Mac reaches (`R_GetImageBuffer`, `R_Hunk_Begin`, `SND_setup`) | Local | The first was found on the emulator (commit aa88b4d) |

## Not done

- ETL's `Com_Printf` overflow fix (`c10805a4`), `Q_ColorizeString` (`89647300`), autocomplete
  (`13f30fcb`) and `COM_ParseExt` (`22c68029`) are not reviewed against this tree yet.
- `sv_userInfoFloodProtect` (`f5258c7d`) and the other server-side ETL fixes matter only for a
  listen server and are not ported.
- Renderer, BSP, model, image and zip loaders take pk3 content from a server. Q3A has a long
  validation series (`docs/review-*` there). None is ported. **A malicious server's pk3
  remains the largest open risk**, and `sv_pure` does not defend a client against it.
- `cm_optimizePatchPlanes` is not followed (E5). When it is, read it from `systemInfo`: the
  cvar keeps the first server's value.

- No sanitizer run of the whole engine. Only `msg.c`, `huffman.c` and `fs_path.c` have one.

## Decided, and stricter than upstream

`challengeResponse` is taken only from the IP the client asked. ET: Legacy takes it
from anyone, so a proxy can hand a connection to another address. Here that is refused,
because a forged one moves the connection to the sender. A multi-homed server that answers
from another address without `net_ip` set will not connect, but most home NATs would drop
that reply too.
