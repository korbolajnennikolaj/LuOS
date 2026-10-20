// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "isr.h"

extern void isr_handler(struct registers* regs, void *fx);

#define ISR_NOERR(n) \
    __attribute__((naked)) static void isr_vec_##n(void) { \
        asm volatile("push $0\n\tpush $" #n "\n\tjmp isr_common_stub\n\t"); \
    }

#define ISR_ERR(n) \
    __attribute__((naked)) static void isr_vec_##n(void) { \
        asm volatile("push $" #n "\n\tjmp isr_common_stub\n\t"); \
    }

ISR_NOERR(0)
ISR_NOERR(1)
ISR_NOERR(2)
ISR_NOERR(3)
ISR_NOERR(4)
ISR_NOERR(5)
ISR_NOERR(6)
ISR_NOERR(7)
ISR_ERR(8)
ISR_NOERR(9)
ISR_ERR(10)
ISR_ERR(11)
ISR_ERR(12)
ISR_ERR(13)
ISR_ERR(14)
ISR_NOERR(15)
ISR_NOERR(16)
ISR_ERR(17)
ISR_NOERR(18)
ISR_NOERR(19)
ISR_NOERR(20)
ISR_ERR(21)
ISR_NOERR(22)
ISR_NOERR(23)
ISR_NOERR(24)
ISR_NOERR(25)
ISR_NOERR(26)
ISR_NOERR(27)
ISR_NOERR(28)
ISR_ERR(29)
ISR_ERR(30)
ISR_NOERR(31)
ISR_NOERR(32)
ISR_NOERR(33)
ISR_NOERR(34)
ISR_NOERR(35)
ISR_NOERR(36)
ISR_NOERR(37)
ISR_NOERR(38)
ISR_NOERR(39)
ISR_NOERR(40)
ISR_NOERR(41)
ISR_NOERR(42)
ISR_NOERR(43)
ISR_NOERR(44)
ISR_NOERR(45)
ISR_NOERR(46)
ISR_NOERR(47)
ISR_NOERR(48)
ISR_NOERR(49)
ISR_NOERR(50)
ISR_NOERR(51)
ISR_NOERR(52)
ISR_NOERR(53)
ISR_NOERR(54)
ISR_NOERR(55)
ISR_NOERR(56)
ISR_NOERR(57)
ISR_NOERR(58)
ISR_NOERR(59)
ISR_NOERR(60)
ISR_NOERR(61)
ISR_NOERR(62)
ISR_NOERR(63)
ISR_NOERR(64)
ISR_NOERR(65)
ISR_NOERR(66)
ISR_NOERR(67)
ISR_NOERR(68)
ISR_NOERR(69)
ISR_NOERR(70)
ISR_NOERR(71)
ISR_NOERR(72)
ISR_NOERR(73)
ISR_NOERR(74)
ISR_NOERR(75)
ISR_NOERR(76)
ISR_NOERR(77)
ISR_NOERR(78)
ISR_NOERR(79)
ISR_NOERR(80)
ISR_NOERR(81)
ISR_NOERR(82)
ISR_NOERR(83)
ISR_NOERR(84)
ISR_NOERR(85)
ISR_NOERR(86)
ISR_NOERR(87)
ISR_NOERR(88)
ISR_NOERR(89)
ISR_NOERR(90)
ISR_NOERR(91)
ISR_NOERR(92)
ISR_NOERR(93)
ISR_NOERR(94)
ISR_NOERR(95)
ISR_NOERR(96)
ISR_NOERR(97)
ISR_NOERR(98)
ISR_NOERR(99)
ISR_NOERR(100)
ISR_NOERR(101)
ISR_NOERR(102)
ISR_NOERR(103)
ISR_NOERR(104)
ISR_NOERR(105)
ISR_NOERR(106)
ISR_NOERR(107)
ISR_NOERR(108)
ISR_NOERR(109)
ISR_NOERR(110)
ISR_NOERR(111)
ISR_NOERR(112)
ISR_NOERR(113)
ISR_NOERR(114)
ISR_NOERR(115)
ISR_NOERR(116)
ISR_NOERR(117)
ISR_NOERR(118)
ISR_NOERR(119)
ISR_NOERR(120)
ISR_NOERR(121)
ISR_NOERR(122)
ISR_NOERR(123)
ISR_NOERR(124)
ISR_NOERR(125)
ISR_NOERR(126)
ISR_NOERR(127)
ISR_NOERR(128)
ISR_NOERR(129)
ISR_NOERR(130)
ISR_NOERR(131)
ISR_NOERR(132)
ISR_NOERR(133)
ISR_NOERR(134)
ISR_NOERR(135)
ISR_NOERR(136)
ISR_NOERR(137)
ISR_NOERR(138)
ISR_NOERR(139)
ISR_NOERR(140)
ISR_NOERR(141)
ISR_NOERR(142)
ISR_NOERR(143)
ISR_NOERR(144)
ISR_NOERR(145)
ISR_NOERR(146)
ISR_NOERR(147)
ISR_NOERR(148)
ISR_NOERR(149)
ISR_NOERR(150)
ISR_NOERR(151)
ISR_NOERR(152)
ISR_NOERR(153)
ISR_NOERR(154)
ISR_NOERR(155)
ISR_NOERR(156)
ISR_NOERR(157)
ISR_NOERR(158)
ISR_NOERR(159)
ISR_NOERR(160)
ISR_NOERR(161)
ISR_NOERR(162)
ISR_NOERR(163)
ISR_NOERR(164)
ISR_NOERR(165)
ISR_NOERR(166)
ISR_NOERR(167)
ISR_NOERR(168)
ISR_NOERR(169)
ISR_NOERR(170)
ISR_NOERR(171)
ISR_NOERR(172)
ISR_NOERR(173)
ISR_NOERR(174)
ISR_NOERR(175)
ISR_NOERR(176)
ISR_NOERR(177)
ISR_NOERR(178)
ISR_NOERR(179)
ISR_NOERR(180)
ISR_NOERR(181)
ISR_NOERR(182)
ISR_NOERR(183)
ISR_NOERR(184)
ISR_NOERR(185)
ISR_NOERR(186)
ISR_NOERR(187)
ISR_NOERR(188)
ISR_NOERR(189)
ISR_NOERR(190)
ISR_NOERR(191)
ISR_NOERR(192)
ISR_NOERR(193)
ISR_NOERR(194)
ISR_NOERR(195)
ISR_NOERR(196)
ISR_NOERR(197)
ISR_NOERR(198)
ISR_NOERR(199)
ISR_NOERR(200)
ISR_NOERR(201)
ISR_NOERR(202)
ISR_NOERR(203)
ISR_NOERR(204)
ISR_NOERR(205)
ISR_NOERR(206)
ISR_NOERR(207)
ISR_NOERR(208)
ISR_NOERR(209)
ISR_NOERR(210)
ISR_NOERR(211)
ISR_NOERR(212)
ISR_NOERR(213)
ISR_NOERR(214)
ISR_NOERR(215)
ISR_NOERR(216)
ISR_NOERR(217)
ISR_NOERR(218)
ISR_NOERR(219)
ISR_NOERR(220)
ISR_NOERR(221)
ISR_NOERR(222)
ISR_NOERR(223)
ISR_NOERR(224)
ISR_NOERR(225)
ISR_NOERR(226)
ISR_NOERR(227)
ISR_NOERR(228)
ISR_NOERR(229)
ISR_NOERR(230)
ISR_NOERR(231)
ISR_NOERR(232)
ISR_NOERR(233)
ISR_NOERR(234)
ISR_NOERR(235)
ISR_NOERR(236)
ISR_NOERR(237)
ISR_NOERR(238)
ISR_NOERR(239)
ISR_NOERR(240)
ISR_NOERR(241)
ISR_NOERR(242)
ISR_NOERR(243)
ISR_NOERR(244)
ISR_NOERR(245)
ISR_NOERR(246)
ISR_NOERR(247)
ISR_NOERR(248)
ISR_NOERR(249)
ISR_NOERR(250)
ISR_NOERR(251)
ISR_NOERR(252)
ISR_NOERR(253)
ISR_NOERR(254)
ISR_NOERR(255)

void *const isr_stub_table[256] = {
    isr_vec_0, isr_vec_1, isr_vec_2, isr_vec_3, isr_vec_4, isr_vec_5, isr_vec_6, isr_vec_7,
    isr_vec_8, isr_vec_9, isr_vec_10, isr_vec_11, isr_vec_12, isr_vec_13, isr_vec_14, isr_vec_15,
    isr_vec_16, isr_vec_17, isr_vec_18, isr_vec_19, isr_vec_20, isr_vec_21, isr_vec_22, isr_vec_23,
    isr_vec_24, isr_vec_25, isr_vec_26, isr_vec_27, isr_vec_28, isr_vec_29, isr_vec_30, isr_vec_31,
    isr_vec_32, isr_vec_33, isr_vec_34, isr_vec_35, isr_vec_36, isr_vec_37, isr_vec_38, isr_vec_39,
    isr_vec_40, isr_vec_41, isr_vec_42, isr_vec_43, isr_vec_44, isr_vec_45, isr_vec_46, isr_vec_47,
    isr_vec_48, isr_vec_49, isr_vec_50, isr_vec_51, isr_vec_52, isr_vec_53, isr_vec_54, isr_vec_55,
    isr_vec_56, isr_vec_57, isr_vec_58, isr_vec_59, isr_vec_60, isr_vec_61, isr_vec_62, isr_vec_63,
    isr_vec_64, isr_vec_65, isr_vec_66, isr_vec_67, isr_vec_68, isr_vec_69, isr_vec_70, isr_vec_71,
    isr_vec_72, isr_vec_73, isr_vec_74, isr_vec_75, isr_vec_76, isr_vec_77, isr_vec_78, isr_vec_79,
    isr_vec_80, isr_vec_81, isr_vec_82, isr_vec_83, isr_vec_84, isr_vec_85, isr_vec_86, isr_vec_87,
    isr_vec_88, isr_vec_89, isr_vec_90, isr_vec_91, isr_vec_92, isr_vec_93, isr_vec_94, isr_vec_95,
    isr_vec_96, isr_vec_97, isr_vec_98, isr_vec_99, isr_vec_100, isr_vec_101, isr_vec_102, isr_vec_103,
    isr_vec_104, isr_vec_105, isr_vec_106, isr_vec_107, isr_vec_108, isr_vec_109, isr_vec_110, isr_vec_111,
    isr_vec_112, isr_vec_113, isr_vec_114, isr_vec_115, isr_vec_116, isr_vec_117, isr_vec_118, isr_vec_119,
    isr_vec_120, isr_vec_121, isr_vec_122, isr_vec_123, isr_vec_124, isr_vec_125, isr_vec_126, isr_vec_127,
    isr_vec_128, isr_vec_129, isr_vec_130, isr_vec_131, isr_vec_132, isr_vec_133, isr_vec_134, isr_vec_135,
    isr_vec_136, isr_vec_137, isr_vec_138, isr_vec_139, isr_vec_140, isr_vec_141, isr_vec_142, isr_vec_143,
    isr_vec_144, isr_vec_145, isr_vec_146, isr_vec_147, isr_vec_148, isr_vec_149, isr_vec_150, isr_vec_151,
    isr_vec_152, isr_vec_153, isr_vec_154, isr_vec_155, isr_vec_156, isr_vec_157, isr_vec_158, isr_vec_159,
    isr_vec_160, isr_vec_161, isr_vec_162, isr_vec_163, isr_vec_164, isr_vec_165, isr_vec_166, isr_vec_167,
    isr_vec_168, isr_vec_169, isr_vec_170, isr_vec_171, isr_vec_172, isr_vec_173, isr_vec_174, isr_vec_175,
    isr_vec_176, isr_vec_177, isr_vec_178, isr_vec_179, isr_vec_180, isr_vec_181, isr_vec_182, isr_vec_183,
    isr_vec_184, isr_vec_185, isr_vec_186, isr_vec_187, isr_vec_188, isr_vec_189, isr_vec_190, isr_vec_191,
    isr_vec_192, isr_vec_193, isr_vec_194, isr_vec_195, isr_vec_196, isr_vec_197, isr_vec_198, isr_vec_199,
    isr_vec_200, isr_vec_201, isr_vec_202, isr_vec_203, isr_vec_204, isr_vec_205, isr_vec_206, isr_vec_207,
    isr_vec_208, isr_vec_209, isr_vec_210, isr_vec_211, isr_vec_212, isr_vec_213, isr_vec_214, isr_vec_215,
    isr_vec_216, isr_vec_217, isr_vec_218, isr_vec_219, isr_vec_220, isr_vec_221, isr_vec_222, isr_vec_223,
    isr_vec_224, isr_vec_225, isr_vec_226, isr_vec_227, isr_vec_228, isr_vec_229, isr_vec_230, isr_vec_231,
    isr_vec_232, isr_vec_233, isr_vec_234, isr_vec_235, isr_vec_236, isr_vec_237, isr_vec_238, isr_vec_239,
    isr_vec_240, isr_vec_241, isr_vec_242, isr_vec_243, isr_vec_244, isr_vec_245, isr_vec_246, isr_vec_247,
    isr_vec_248, isr_vec_249, isr_vec_250, isr_vec_251, isr_vec_252, isr_vec_253, isr_vec_254, isr_vec_255,
};

__attribute__((naked)) void isr_common_stub(void) {
    asm volatile(
        "push %rax\n\t"
        "push %rbx\n\t"
        "push %rcx\n\t"
        "push %rdx\n\t"
        "push %rsi\n\t"
        "push %rdi\n\t"
        "push %rbp\n\t"
        "push %r8\n\t"
        "push %r9\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"

        "cld\n\t"
        "mov %rsp, %rdi\n\t"
        "mov %rsp, %rbx\n\t"
        "sub fpu_save_size(%rip), %rsp\n\t"
        "and $-64, %rsp\n\t"
        "cmpb $0, fpu_use_xsave(%rip)\n\t"
        "je 1f\n\t"
        "xor %eax, %eax\n\t"
        "mov %rax, 512(%rsp)\n\t"
        "mov %rax, 520(%rsp)\n\t"
        "mov %rax, 528(%rsp)\n\t"
        "mov %rax, 536(%rsp)\n\t"
        "mov %rax, 544(%rsp)\n\t"
        "mov %rax, 552(%rsp)\n\t"
        "mov %rax, 560(%rsp)\n\t"
        "mov %rax, 568(%rsp)\n\t"
        "mov $-1, %eax\n\t"
        "mov $-1, %edx\n\t"
        "xsave64 (%rsp)\n\t"
        "jmp 2f\n"
        "1:\n\t"
        "fxsave (%rsp)\n"
        "2:\n\t"
        "mov %rsp, %rsi\n\t"
        "call isr_handler\n\t"
        "cmpb $0, fpu_use_xsave(%rip)\n\t"
        "je 3f\n\t"
        "mov $-1, %eax\n\t"
        "mov $-1, %edx\n\t"
        "xrstor64 (%rsp)\n\t"
        "jmp 4f\n"
        "3:\n\t"
        "fxrstor (%rsp)\n"
        "4:\n\t"
        "mov %rbx, %rsp\n\t"

        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %r9\n\t"
        "pop %r8\n\t"
        "pop %rbp\n\t"
        "pop %rdi\n\t"
        "pop %rsi\n\t"
        "pop %rdx\n\t"
        "pop %rcx\n\t"
        "pop %rbx\n\t"
        "pop %rax\n\t"
        "add $16, %rsp\n\t"
        "iretq\n\t"
    );
}
