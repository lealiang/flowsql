/*
 * Copyright (C) 2020-06 - flowSQL
 *
 *
 * Licensed under the MIT License. See LICENSE file in the project root
 * for full license information.
 *
 *
 * Author       : LIHUO
 * Date         : 2020-11-14 22:13:14
 * LastEditors  : LIHUO
 * LastEditTime : 2026-02-25 12:00:00
 */
#ifdef _MSC_VER
#define GET_ARG_COUNT_(...)                                                                                            \
    PASTE(GET_ARG_COUNT_I(                                                                                             \
              __VA_ARGS__, 299, 298, 297, 296, 295, 294, 293, 292, 291, 290, 289, 288, 287, 286, 285, 284, 283, 282,   \
              281, 280, 279, 278, 277, 276, 275, 274, 273, 272, 271, 270, 269, 268, 267, 266, 265, 264, 263, 262, 261, \
              260, 259, 258, 257, 256, 255, 254, 253, 252, 251, 250, 249, 248, 247, 246, 245, 244, 243, 242, 241, 240, \
              239, 238, 237, 236, 235, 234, 233, 232, 231, 230, 229, 228, 227, 226, 225, 224, 223, 222, 221, 220, 219, \
              218, 217, 216, 215, 214, 213, 212, 211, 210, 209, 208, 207, 206, 205, 204, 203, 202, 201, 200, 199, 198, \
              197, 196, 195, 194, 193, 192, 191, 190, 189, 188, 187, 186, 185, 184, 183, 182, 181, 180, 179, 178, 177, \
              176, 175, 174, 173, 172, 171, 170, 169, 168, 167, 166, 165, 164, 163, 162, 161, 160, 159, 158, 157, 156, \
              155, 154, 153, 152, 151, 150, 149, 148, 147, 146, 145, 144, 143, 142, 141, 140, 139, 138, 137, 136, 135, \
              134, 133, 132, 131, 130, 129, 128, 127, 126, 125, 124, 123, 122, 121, 120, 119, 118, 117, 116, 115, 114, \
              113, 112, 111, 110, 109, 108, 107, 106, 105, 104, 103, 102, 101, 100, 99, 98, 97, 96, 95, 94, 93, 92,    \
              91, 90, 89, 88, 87, 86, 85, 84, 83, 82, 81, 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66,  \
              65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40,  \
              39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14,  \
              13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, ),                                                         \
          __CUB_empty())

#else

#define GET_ARG_COUNT_(...)                                                                                           \
    GET_ARG_COUNT_I(                                                                                                  \
        __VA_ARGS__, 299, 298, 297, 296, 295, 294, 293, 292, 291, 290, 289, 288, 287, 286, 285, 284, 283, 282, 281,   \
        280, 279, 278, 277, 276, 275, 274, 273, 272, 271, 270, 269, 268, 267, 266, 265, 264, 263, 262, 261, 260, 259, \
        258, 257, 256, 255, 254, 253, 252, 251, 250, 249, 248, 247, 246, 245, 244, 243, 242, 241, 240, 239, 238, 237, \
        236, 235, 234, 233, 232, 231, 230, 229, 228, 227, 226, 225, 224, 223, 222, 221, 220, 219, 218, 217, 216, 215, \
        214, 213, 212, 211, 210, 209, 208, 207, 206, 205, 204, 203, 202, 201, 200, 199, 198, 197, 196, 195, 194, 193, \
        192, 191, 190, 189, 188, 187, 186, 185, 184, 183, 182, 181, 180, 179, 178, 177, 176, 175, 174, 173, 172, 171, \
        170, 169, 168, 167, 166, 165, 164, 163, 162, 161, 160, 159, 158, 157, 156, 155, 154, 153, 152, 151, 150, 149, \
        148, 147, 146, 145, 144, 143, 142, 141, 140, 139, 138, 137, 136, 135, 134, 133, 132, 131, 130, 129, 128, 127, \
        126, 125, 124, 123, 122, 121, 120, 119, 118, 117, 116, 115, 114, 113, 112, 111, 110, 109, 108, 107, 106, 105, \
        104, 103, 102, 101, 100, 99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, 87, 86, 85, 84, 83, 82, 81, 80, 79,  \
        78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52,   \
        51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25,   \
        24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, )

#endif

#define GET_ARG_COUNT(...) GET_ARG_COUNT_(__dummy__, ##__VA_ARGS__)
#define GET_ARG_COUNT_I(                                                                                               \
    e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, \
    e25, e26, e27, e28, e29, e30, e31, e32, e33, e34, e35, e36, e37, e38, e39, e40, e41, e42, e43, e44, e45, e46, e47, \
    e48, e49, e50, e51, e52, e53, e54, e55, e56, e57, e58, e59, e60, e61, e62, e63, e64, e65, e66, e67, e68, e69, e70, \
    e71, e72, e73, e74, e75, e76, e77, e78, e79, e80, e81, e82, e83, e84, e85, e86, e87, e88, e89, e90, e91, e92, e93, \
    e94, e95, e96, e97, e98, e99, e100, e101, e102, e103, e104, e105, e106, e107, e108, e109, e110, e111, e112, e113,  \
    e114, e115, e116, e117, e118, e119, e120, e121, e122, e123, e124, e125, e126, e127, e128, e129, e130, e131, e132,  \
    e133, e134, e135, e136, e137, e138, e139, e140, e141, e142, e143, e144, e145, e146, e147, e148, e149, e150, e151,  \
    e152, e153, e154, e155, e156, e157, e158, e159, e160, e161, e162, e163, e164, e165, e166, e167, e168, e169, e170,  \
    e171, e172, e173, e174, e175, e176, e177, e178, e179, e180, e181, e182, e183, e184, e185, e186, e187, e188, e189,  \
    e190, e191, e192, e193, e194, e195, e196, e197, e198, e199, e200, e201, e202, e203, e204, e205, e206, e207, e208,  \
    e209, e210, e211, e212, e213, e214, e215, e216, e217, e218, e219, e220, e221, e222, e223, e224, e225, e226, e227,  \
    e228, e229, e230, e231, e232, e233, e234, e235, e236, e237, e238, e239, e240, e241, e242, e243, e244, e245, e246,  \
    e247, e248, e249, e250, e251, e252, e253, e254, e255, e256, e257, e258, e259, e260, e261, e262, e263, e264, e265,  \
    e266, e267, e268, e269, e270, e271, e272, e273, e274, e275, e276, e277, e278, e279, e280, e281, e282, e283, e284,  \
    e285, e286, e287, e288, e289, e290, e291, e292, e293, e294, e295, e296, e297, e298, e299, size, ...)               \
    size

#define REPEAT_0(func, i, arg)
#define REPEAT_1(func, i, arg) func(i, arg)
#define REPEAT_2(func, i, arg, ...) func(i, arg) REPEAT_1(func, i + 1, __VA_ARGS__)
#define REPEAT_3(func, i, arg, ...) func(i, arg) REPEAT_2(func, i + 1, __VA_ARGS__)
#define REPEAT_4(func, i, arg, ...) func(i, arg) REPEAT_3(func, i + 1, __VA_ARGS__)
#define REPEAT_5(func, i, arg, ...) func(i, arg) REPEAT_4(func, i + 1, __VA_ARGS__)
#define REPEAT_6(func, i, arg, ...) func(i, arg) REPEAT_5(func, i + 1, __VA_ARGS__)
#define REPEAT_7(func, i, arg, ...) func(i, arg) REPEAT_6(func, i + 1, __VA_ARGS__)
#define REPEAT_8(func, i, arg, ...) func(i, arg) REPEAT_7(func, i + 1, __VA_ARGS__)
#define REPEAT_9(func, i, arg, ...) func(i, arg) REPEAT_8(func, i + 1, __VA_ARGS__)
#define REPEAT_10(func, i, arg, ...) func(i, arg) REPEAT_9(func, i + 1, __VA_ARGS__)
#define REPEAT_11(func, i, arg, ...) func(i, arg) REPEAT_10(func, i + 1, __VA_ARGS__)
#define REPEAT_12(func, i, arg, ...) func(i, arg) REPEAT_11(func, i + 1, __VA_ARGS__)
#define REPEAT_13(func, i, arg, ...) func(i, arg) REPEAT_12(func, i + 1, __VA_ARGS__)
#define REPEAT_14(func, i, arg, ...) func(i, arg) REPEAT_13(func, i + 1, __VA_ARGS__)
#define REPEAT_15(func, i, arg, ...) func(i, arg) REPEAT_14(func, i + 1, __VA_ARGS__)
#define REPEAT_16(func, i, arg, ...) func(i, arg) REPEAT_15(func, i + 1, __VA_ARGS__)
#define REPEAT_17(func, i, arg, ...) func(i, arg) REPEAT_16(func, i + 1, __VA_ARGS__)
#define REPEAT_18(func, i, arg, ...) func(i, arg) REPEAT_17(func, i + 1, __VA_ARGS__)
#define REPEAT_19(func, i, arg, ...) func(i, arg) REPEAT_18(func, i + 1, __VA_ARGS__)
#define REPEAT_20(func, i, arg, ...) func(i, arg) REPEAT_19(func, i + 1, __VA_ARGS__)
#define REPEAT_21(func, i, arg, ...) func(i, arg) REPEAT_20(func, i + 1, __VA_ARGS__)
#define REPEAT_22(func, i, arg, ...) func(i, arg) REPEAT_21(func, i + 1, __VA_ARGS__)
#define REPEAT_23(func, i, arg, ...) func(i, arg) REPEAT_22(func, i + 1, __VA_ARGS__)
#define REPEAT_24(func, i, arg, ...) func(i, arg) REPEAT_23(func, i + 1, __VA_ARGS__)
#define REPEAT_25(func, i, arg, ...) func(i, arg) REPEAT_24(func, i + 1, __VA_ARGS__)
#define REPEAT_26(func, i, arg, ...) func(i, arg) REPEAT_25(func, i + 1, __VA_ARGS__)
#define REPEAT_27(func, i, arg, ...) func(i, arg) REPEAT_26(func, i + 1, __VA_ARGS__)
#define REPEAT_28(func, i, arg, ...) func(i, arg) REPEAT_27(func, i + 1, __VA_ARGS__)
#define REPEAT_29(func, i, arg, ...) func(i, arg) REPEAT_28(func, i + 1, __VA_ARGS__)
#define REPEAT_30(func, i, arg, ...) func(i, arg) REPEAT_29(func, i + 1, __VA_ARGS__)
#define REPEAT_31(func, i, arg, ...) func(i, arg) REPEAT_30(func, i + 1, __VA_ARGS__)
#define REPEAT_32(func, i, arg, ...) func(i, arg) REPEAT_31(func, i + 1, __VA_ARGS__)
#define REPEAT_33(func, i, arg, ...) func(i, arg) REPEAT_32(func, i + 1, __VA_ARGS__)
#define REPEAT_34(func, i, arg, ...) func(i, arg) REPEAT_33(func, i + 1, __VA_ARGS__)
#define REPEAT_35(func, i, arg, ...) func(i, arg) REPEAT_34(func, i + 1, __VA_ARGS__)
#define REPEAT_36(func, i, arg, ...) func(i, arg) REPEAT_35(func, i + 1, __VA_ARGS__)
#define REPEAT_37(func, i, arg, ...) func(i, arg) REPEAT_36(func, i + 1, __VA_ARGS__)
#define REPEAT_38(func, i, arg, ...) func(i, arg) REPEAT_37(func, i + 1, __VA_ARGS__)
#define REPEAT_39(func, i, arg, ...) func(i, arg) REPEAT_38(func, i + 1, __VA_ARGS__)
#define REPEAT_40(func, i, arg, ...) func(i, arg) REPEAT_39(func, i + 1, __VA_ARGS__)
#define REPEAT_41(func, i, arg, ...) func(i, arg) REPEAT_40(func, i + 1, __VA_ARGS__)
#define REPEAT_42(func, i, arg, ...) func(i, arg) REPEAT_41(func, i + 1, __VA_ARGS__)
#define REPEAT_43(func, i, arg, ...) func(i, arg) REPEAT_42(func, i + 1, __VA_ARGS__)
#define REPEAT_44(func, i, arg, ...) func(i, arg) REPEAT_43(func, i + 1, __VA_ARGS__)
#define REPEAT_45(func, i, arg, ...) func(i, arg) REPEAT_44(func, i + 1, __VA_ARGS__)
#define REPEAT_46(func, i, arg, ...) func(i, arg) REPEAT_45(func, i + 1, __VA_ARGS__)
#define REPEAT_47(func, i, arg, ...) func(i, arg) REPEAT_46(func, i + 1, __VA_ARGS__)
#define REPEAT_48(func, i, arg, ...) func(i, arg) REPEAT_47(func, i + 1, __VA_ARGS__)
#define REPEAT_49(func, i, arg, ...) func(i, arg) REPEAT_48(func, i + 1, __VA_ARGS__)
#define REPEAT_50(func, i, arg, ...) func(i, arg) REPEAT_49(func, i + 1, __VA_ARGS__)
#define REPEAT_51(func, i, arg, ...) func(i, arg) REPEAT_50(func, i + 1, __VA_ARGS__)
#define REPEAT_52(func, i, arg, ...) func(i, arg) REPEAT_51(func, i + 1, __VA_ARGS__)
#define REPEAT_53(func, i, arg, ...) func(i, arg) REPEAT_52(func, i + 1, __VA_ARGS__)
#define REPEAT_54(func, i, arg, ...) func(i, arg) REPEAT_53(func, i + 1, __VA_ARGS__)
#define REPEAT_55(func, i, arg, ...) func(i, arg) REPEAT_54(func, i + 1, __VA_ARGS__)
#define REPEAT_56(func, i, arg, ...) func(i, arg) REPEAT_55(func, i + 1, __VA_ARGS__)
#define REPEAT_57(func, i, arg, ...) func(i, arg) REPEAT_56(func, i + 1, __VA_ARGS__)
#define REPEAT_58(func, i, arg, ...) func(i, arg) REPEAT_57(func, i + 1, __VA_ARGS__)
#define REPEAT_59(func, i, arg, ...) func(i, arg) REPEAT_58(func, i + 1, __VA_ARGS__)
#define REPEAT_60(func, i, arg, ...) func(i, arg) REPEAT_59(func, i + 1, __VA_ARGS__)
#define REPEAT_61(func, i, arg, ...) func(i, arg) REPEAT_60(func, i + 1, __VA_ARGS__)
#define REPEAT_62(func, i, arg, ...) func(i, arg) REPEAT_61(func, i + 1, __VA_ARGS__)
#define REPEAT_63(func, i, arg, ...) func(i, arg) REPEAT_62(func, i + 1, __VA_ARGS__)
#define REPEAT_64(func, i, arg, ...) func(i, arg) REPEAT_63(func, i + 1, __VA_ARGS__)
#define REPEAT_65(func, i, arg, ...) func(i, arg) REPEAT_64(func, i + 1, __VA_ARGS__)
#define REPEAT_66(func, i, arg, ...) func(i, arg) REPEAT_65(func, i + 1, __VA_ARGS__)
#define REPEAT_67(func, i, arg, ...) func(i, arg) REPEAT_66(func, i + 1, __VA_ARGS__)
#define REPEAT_68(func, i, arg, ...) func(i, arg) REPEAT_67(func, i + 1, __VA_ARGS__)
#define REPEAT_69(func, i, arg, ...) func(i, arg) REPEAT_68(func, i + 1, __VA_ARGS__)
#define REPEAT_70(func, i, arg, ...) func(i, arg) REPEAT_69(func, i + 1, __VA_ARGS__)
#define REPEAT_71(func, i, arg, ...) func(i, arg) REPEAT_70(func, i + 1, __VA_ARGS__)
#define REPEAT_72(func, i, arg, ...) func(i, arg) REPEAT_71(func, i + 1, __VA_ARGS__)
#define REPEAT_73(func, i, arg, ...) func(i, arg) REPEAT_72(func, i + 1, __VA_ARGS__)
#define REPEAT_74(func, i, arg, ...) func(i, arg) REPEAT_73(func, i + 1, __VA_ARGS__)
#define REPEAT_75(func, i, arg, ...) func(i, arg) REPEAT_74(func, i + 1, __VA_ARGS__)
#define REPEAT_76(func, i, arg, ...) func(i, arg) REPEAT_75(func, i + 1, __VA_ARGS__)
#define REPEAT_77(func, i, arg, ...) func(i, arg) REPEAT_76(func, i + 1, __VA_ARGS__)
#define REPEAT_78(func, i, arg, ...) func(i, arg) REPEAT_77(func, i + 1, __VA_ARGS__)
#define REPEAT_79(func, i, arg, ...) func(i, arg) REPEAT_78(func, i + 1, __VA_ARGS__)
#define REPEAT_80(func, i, arg, ...) func(i, arg) REPEAT_79(func, i + 1, __VA_ARGS__)
#define REPEAT_81(func, i, arg, ...) func(i, arg) REPEAT_80(func, i + 1, __VA_ARGS__)
#define REPEAT_82(func, i, arg, ...) func(i, arg) REPEAT_81(func, i + 1, __VA_ARGS__)
#define REPEAT_83(func, i, arg, ...) func(i, arg) REPEAT_82(func, i + 1, __VA_ARGS__)
#define REPEAT_84(func, i, arg, ...) func(i, arg) REPEAT_83(func, i + 1, __VA_ARGS__)
#define REPEAT_85(func, i, arg, ...) func(i, arg) REPEAT_84(func, i + 1, __VA_ARGS__)
#define REPEAT_86(func, i, arg, ...) func(i, arg) REPEAT_85(func, i + 1, __VA_ARGS__)
#define REPEAT_87(func, i, arg, ...) func(i, arg) REPEAT_86(func, i + 1, __VA_ARGS__)
#define REPEAT_88(func, i, arg, ...) func(i, arg) REPEAT_87(func, i + 1, __VA_ARGS__)
#define REPEAT_89(func, i, arg, ...) func(i, arg) REPEAT_88(func, i + 1, __VA_ARGS__)
#define REPEAT_90(func, i, arg, ...) func(i, arg) REPEAT_89(func, i + 1, __VA_ARGS__)
#define REPEAT_91(func, i, arg, ...) func(i, arg) REPEAT_90(func, i + 1, __VA_ARGS__)
#define REPEAT_92(func, i, arg, ...) func(i, arg) REPEAT_91(func, i + 1, __VA_ARGS__)
#define REPEAT_93(func, i, arg, ...) func(i, arg) REPEAT_92(func, i + 1, __VA_ARGS__)
#define REPEAT_94(func, i, arg, ...) func(i, arg) REPEAT_93(func, i + 1, __VA_ARGS__)
#define REPEAT_95(func, i, arg, ...) func(i, arg) REPEAT_94(func, i + 1, __VA_ARGS__)
#define REPEAT_96(func, i, arg, ...) func(i, arg) REPEAT_95(func, i + 1, __VA_ARGS__)
#define REPEAT_97(func, i, arg, ...) func(i, arg) REPEAT_96(func, i + 1, __VA_ARGS__)
#define REPEAT_98(func, i, arg, ...) func(i, arg) REPEAT_97(func, i + 1, __VA_ARGS__)
#define REPEAT_99(func, i, arg, ...) func(i, arg) REPEAT_98(func, i + 1, __VA_ARGS__)
#define REPEAT_100(func, i, arg, ...) func(i, arg) REPEAT_99(func, i + 1, __VA_ARGS__)
#define REPEAT_101(func, i, arg, ...) func(i, arg) REPEAT_100(func, i + 1, __VA_ARGS__)
#define REPEAT_102(func, i, arg, ...) func(i, arg) REPEAT_101(func, i + 1, __VA_ARGS__)
#define REPEAT_103(func, i, arg, ...) func(i, arg) REPEAT_102(func, i + 1, __VA_ARGS__)
#define REPEAT_104(func, i, arg, ...) func(i, arg) REPEAT_103(func, i + 1, __VA_ARGS__)
#define REPEAT_105(func, i, arg, ...) func(i, arg) REPEAT_104(func, i + 1, __VA_ARGS__)
#define REPEAT_106(func, i, arg, ...) func(i, arg) REPEAT_105(func, i + 1, __VA_ARGS__)
#define REPEAT_107(func, i, arg, ...) func(i, arg) REPEAT_106(func, i + 1, __VA_ARGS__)
#define REPEAT_108(func, i, arg, ...) func(i, arg) REPEAT_107(func, i + 1, __VA_ARGS__)
#define REPEAT_109(func, i, arg, ...) func(i, arg) REPEAT_108(func, i + 1, __VA_ARGS__)
#define REPEAT_110(func, i, arg, ...) func(i, arg) REPEAT_109(func, i + 1, __VA_ARGS__)
#define REPEAT_111(func, i, arg, ...) func(i, arg) REPEAT_110(func, i + 1, __VA_ARGS__)
#define REPEAT_112(func, i, arg, ...) func(i, arg) REPEAT_111(func, i + 1, __VA_ARGS__)
#define REPEAT_113(func, i, arg, ...) func(i, arg) REPEAT_112(func, i + 1, __VA_ARGS__)
#define REPEAT_114(func, i, arg, ...) func(i, arg) REPEAT_113(func, i + 1, __VA_ARGS__)
#define REPEAT_115(func, i, arg, ...) func(i, arg) REPEAT_114(func, i + 1, __VA_ARGS__)
#define REPEAT_116(func, i, arg, ...) func(i, arg) REPEAT_115(func, i + 1, __VA_ARGS__)
#define REPEAT_117(func, i, arg, ...) func(i, arg) REPEAT_116(func, i + 1, __VA_ARGS__)
#define REPEAT_118(func, i, arg, ...) func(i, arg) REPEAT_117(func, i + 1, __VA_ARGS__)
#define REPEAT_119(func, i, arg, ...) func(i, arg) REPEAT_118(func, i + 1, __VA_ARGS__)
#define REPEAT_120(func, i, arg, ...) func(i, arg) REPEAT_119(func, i + 1, __VA_ARGS__)
#define REPEAT_121(func, i, arg, ...) func(i, arg) REPEAT_120(func, i + 1, __VA_ARGS__)
#define REPEAT_122(func, i, arg, ...) func(i, arg) REPEAT_121(func, i + 1, __VA_ARGS__)
#define REPEAT_123(func, i, arg, ...) func(i, arg) REPEAT_122(func, i + 1, __VA_ARGS__)
#define REPEAT_124(func, i, arg, ...) func(i, arg) REPEAT_123(func, i + 1, __VA_ARGS__)
#define REPEAT_125(func, i, arg, ...) func(i, arg) REPEAT_124(func, i + 1, __VA_ARGS__)
#define REPEAT_126(func, i, arg, ...) func(i, arg) REPEAT_125(func, i + 1, __VA_ARGS__)
#define REPEAT_127(func, i, arg, ...) func(i, arg) REPEAT_126(func, i + 1, __VA_ARGS__)
#define REPEAT_128(func, i, arg, ...) func(i, arg) REPEAT_127(func, i + 1, __VA_ARGS__)
#define REPEAT_129(func, i, arg, ...) func(i, arg) REPEAT_128(func, i + 1, __VA_ARGS__)
#define REPEAT_130(func, i, arg, ...) func(i, arg) REPEAT_129(func, i + 1, __VA_ARGS__)
#define REPEAT_131(func, i, arg, ...) func(i, arg) REPEAT_130(func, i + 1, __VA_ARGS__)
#define REPEAT_132(func, i, arg, ...) func(i, arg) REPEAT_131(func, i + 1, __VA_ARGS__)
#define REPEAT_133(func, i, arg, ...) func(i, arg) REPEAT_132(func, i + 1, __VA_ARGS__)
#define REPEAT_134(func, i, arg, ...) func(i, arg) REPEAT_133(func, i + 1, __VA_ARGS__)
#define REPEAT_135(func, i, arg, ...) func(i, arg) REPEAT_134(func, i + 1, __VA_ARGS__)
#define REPEAT_136(func, i, arg, ...) func(i, arg) REPEAT_135(func, i + 1, __VA_ARGS__)
#define REPEAT_137(func, i, arg, ...) func(i, arg) REPEAT_136(func, i + 1, __VA_ARGS__)
#define REPEAT_138(func, i, arg, ...) func(i, arg) REPEAT_137(func, i + 1, __VA_ARGS__)
#define REPEAT_139(func, i, arg, ...) func(i, arg) REPEAT_138(func, i + 1, __VA_ARGS__)
#define REPEAT_140(func, i, arg, ...) func(i, arg) REPEAT_139(func, i + 1, __VA_ARGS__)

#define STR(x) #x
#define CONCATE(x, y) x##y
#define STRING(x) STR(x)
#define PARE(...) __VA_ARGS__
#define EAT(...)
#define PAIR(x) PARE x  // PAIR((int) x) => PARE(int) x => int x
#define STRIP(x) EAT x  // STRIP((int) x) => EAT(int) x => x
#define PASTE(x, y) CONCATE(x, y)