from double_stub import serve


def double(data, tag):
    return data + data


serve(double)
