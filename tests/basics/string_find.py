print("hello world".find("ll"))
print("hello world".find("ll", None))
print("hello world".find("ll", 1))
print("hello world".find("ll", 1, None))
print("hello world".find("ll", None, None))
print("hello world".find("ll", 1, -1))
print("hello world".find("ll", 1, 1))
print("hello world".find("ll", 1, 2))
print("hello world".find("ll", 1, 3))
print("hello world".find("ll", 1, 4))
print("hello world".find("ll", 1, 5))
print("hello world".find("ll", -100))
print("0000".find('0'))
print("0000".find('0', 0))
print("0000".find('0', 1))
print("0000".find('0', 2))
print("0000".find('0', 3))
print("0000".find('0', 4))
print("0000".find('0', 5))
print("0000".find('-1', 3))
print("0000".find('1', 3))
print("0000".find('1', 4))
print("0000".find('1', 5))
print("aaaaaaaaaaa".find("bbb", 9, 2))
print("".find(""))
print("".find("a"))
print("abc".find("", 1))  # 1
print("abc".find("", 3))  # 3
print("abc".find("", 4))  # -1
print("abc".find("", 9))  # -1
print("abc".find("", -1))  # 2
print("abc".find("", -3))  # 0
print("abc".find("", -5))  # 0
print("abc".find("", 1, 1))  # 1
print("abc".find("", 3, 3))  # 3
print("abc".find("", 3, 4))  # 3
print("abc".find("", 3, 9))  # 3
print("abc".find("", 3, 2))  # -1
print("abc".find("", 3, -1))  # -1
print("abc".find("", 1, -1))  # 1
print("abc".find("", -6, -5))  # 0
print("abc".find("", -6, -9))  # 0

try:
    'abc'.find(1)
except TypeError:
    print('TypeError')
