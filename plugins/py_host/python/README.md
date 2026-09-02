# PythonHostPlugin dependencies

Declare every third-party Python dependency in `pyproject.toml` and commit the
resulting `uv.lock`. The embedded interpreter and standard library come from
vcpkg; uv manages only `site-packages`.

```sh
uv add --project plugins/py_host/python <package>
```

`stage_run` synchronizes the lock automatically. Debug staging rejects native
`.pyd`, `.so`, and `.dll` extensions because ordinary release wheels are not
compatible with a Debug CPython ABI. Keep Debug dependencies pure Python or
provide explicitly built Debug-ABI artifacts. Release staging accepts locked
native wheels.
