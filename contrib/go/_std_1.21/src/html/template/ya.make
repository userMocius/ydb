GO_LIBRARY()
IF (FALSE)
    MESSAGE(FATAL this shall never happen)

ELSEIF (OS_LINUX AND ARCH_X86_64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_LINUX AND ARCH_ARM64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_LINUX AND ARCH_AARCH64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_DARWIN AND ARCH_X86_64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_DARWIN AND ARCH_ARM64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_DARWIN AND ARCH_AARCH64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_WINDOWS AND ARCH_X86_64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_WINDOWS AND ARCH_ARM64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ELSEIF (OS_WINDOWS AND ARCH_AARCH64)
    SRCS(
		attr.go
		attr_string.go
		content.go
		context.go
		css.go
		delim_string.go
		doc.go
		element_string.go
		error.go
		escape.go
		html.go
		js.go
		jsctx_string.go
		state_string.go
		template.go
		transition.go
		url.go
		urlpart_string.go
    )
ENDIF()
END()
