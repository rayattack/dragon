# Written once by `dragon ffi sync` - this file is yours; edit freely.
from double_stub import serve


def double(data, tag):
    return data + data


serve(double)
