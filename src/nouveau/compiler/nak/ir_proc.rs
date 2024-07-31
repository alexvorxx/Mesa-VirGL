// Copyright © 2023 Collabora, Ltd.
// SPDX-License-Identifier: MIT

extern crate proc_macro;
extern crate proc_macro2;
#[macro_use]
extern crate quote;
extern crate syn;

use proc_macro::TokenStream;
use proc_macro2::{Span, TokenStream as TokenStream2};
use syn::*;

fn expr_as_usize(expr: &syn::Expr) -> usize {
    let lit = match expr {
        syn::Expr::Lit(lit) => lit,
        _ => panic!("Expected a literal, found an expression"),
    };
    let lit_int = match &lit.lit {
        syn::Lit::Int(i) => i,
        _ => panic!("Expected a literal integer"),
    };
    assert!(lit.attrs.is_empty());
    lit_int
        .base10_parse()
        .expect("Failed to parse integer literal")
}

fn count_type(ty: &Type, slice_type: &str) -> usize {
    match ty {
        syn::Type::Array(a) => {
            let elems = count_type(a.elem.as_ref(), slice_type);
            if elems > 0 {
                elems * expr_as_usize(&a.len)
            } else {
                0
            }
        }
        syn::Type::Path(p) => {
            if p.qself.is_none() && p.path.is_ident(slice_type) {
                1
            } else {
                0
            }
        }
        _ => 0,
    }
}

fn get_attr(field: &Field, attr_name: &str) -> Option<String> {
    for attr in &field.attrs {
        if let Meta::List(ml) = &attr.meta {
            if ml.path.is_ident(attr_name) {
                return Some(format!("{}", ml.tokens));
            }
        }
    }
    None
}

fn derive_as_slice(
    input: TokenStream,
    slice_type: &str,
    attr_name: &str,
    attr_type: &str,
) -> TokenStream {
    let DeriveInput {
        attrs, ident, data, ..
    } = parse_macro_input!(input);

    match data {
        Data::Struct(s) => {
            let mut has_repr_c = false;
            for attr in attrs {
                match attr.meta {
                    Meta::List(ml) => {
                        if ml.path.is_ident("repr")
                            && format!("{}", ml.tokens) == "C"
                        {
                            has_repr_c = true;
                        }
                    }
                    _ => (),
                }
            }
            assert!(has_repr_c, "Struct must be declared #[repr(C)]");

            let mut first = None;
            let mut count = 0_usize;
            let mut found_last = false;
            let mut attrs = TokenStream2::new();

            if let Fields::Named(named) = s.fields {
                for f in named.named {
                    let f_count = count_type(&f.ty, slice_type);
                    let f_attr = get_attr(&f, &attr_name);

                    if f_count > 0 {
                        assert!(
                            !found_last,
                            "All fields of type {slice_type} must be consecutive",
                        );

                        let attr_type =
                            Ident::new(attr_type, Span::call_site());
                        let f_attr = if let Some(s) = f_attr {
                            let s = syn::parse_str::<Ident>(&s).unwrap();
                            quote! { #attr_type::#s, }
                        } else {
                            quote! { #attr_type::DEFAULT, }
                        };

                        first.get_or_insert(f.ident);
                        for _ in 0..f_count {
                            attrs.extend(f_attr.clone());
                        }
                        count += f_count;
                    } else {
                        assert!(
                            f_attr.is_none(),
                            "{attr_name} attribute is only allowed on {slice_type}"
                        );
                        if !first.is_none() {
                            found_last = true;
                        }
                    }
                }
            } else {
                panic!("Fields are not named");
            }

            let slice_type = Ident::new(slice_type, Span::call_site());
            let attr_type = Ident::new(attr_type, Span::call_site());
            if let Some(first) = first {
                quote! {
                    impl AsSlice<#slice_type> for #ident {
                        type Attr = #attr_type;

                        fn as_slice(&self) -> &[#slice_type] {
                            unsafe {
                                let first = &self.#first as *const #slice_type;
                                std::slice::from_raw_parts(first, #count)
                            }
                        }

                        fn as_mut_slice(&mut self) -> &mut [#slice_type] {
                            unsafe {
                                let first =
                                    &mut self.#first as *mut #slice_type;
                                std::slice::from_raw_parts_mut(first, #count)
                            }
                        }

                        fn attrs(&self) -> AttrList<Self::Attr> {
                            static ATTRS: [#attr_type; #count] = [#attrs];
                            AttrList::Array(&ATTRS)
                        }
                    }
                }
            } else {
                quote! {
                    impl AsSlice<#slice_type> for #ident {
                        type Attr = #attr_type;

                        fn as_slice(&self) -> &[#slice_type] {
                            &[]
                        }

                        fn as_mut_slice(&mut self) -> &mut [#slice_type] {
                            &mut []
                        }

                        fn attrs(&self) -> AttrList<Self::Attr> {
                            AttrList::Uniform(#attr_type::DEFAULT)
                        }
                    }
                }
            }
            .into()
        }
        Data::Enum(e) => {
            let mut as_slice_cases = TokenStream2::new();
            let mut as_mut_slice_cases = TokenStream2::new();
            let mut types_cases = TokenStream2::new();
            let slice_type = Ident::new(slice_type, Span::call_site());
            let attr_type = Ident::new(attr_type, Span::call_site());
            for v in e.variants {
                let case = v.ident;
                as_slice_cases.extend(quote! {
                    #ident::#case(x) => AsSlice::<#slice_type>::as_slice(x),
                });
                as_mut_slice_cases.extend(quote! {
                    #ident::#case(x) => AsSlice::<#slice_type>::as_mut_slice(x),
                });
                types_cases.extend(quote! {
                    #ident::#case(x) => AsSlice::<#slice_type>::attrs(x),
                });
            }
            quote! {
                impl AsSlice<#slice_type> for #ident {
                    type Attr = #attr_type;

                    fn as_slice(&self) -> &[#slice_type] {
                        match self {
                            #as_slice_cases
                        }
                    }

                    fn as_mut_slice(&mut self) -> &mut [#slice_type] {
                        match self {
                            #as_mut_slice_cases
                        }
                    }

                    fn attrs(&self) -> AttrList<Self::Attr> {
                        match self {
                            #types_cases
                        }
                    }
                }
            }
            .into()
        }
        _ => panic!("Not a struct type"),
    }
}

#[proc_macro_derive(SrcsAsSlice, attributes(src_type))]
pub fn derive_srcs_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "Src", "src_type", "SrcType")
}

#[proc_macro_derive(DstsAsSlice, attributes(dst_type))]
pub fn derive_dsts_as_slice(input: TokenStream) -> TokenStream {
    derive_as_slice(input, "Dst", "dst_type", "DstType")
}

#[proc_macro_derive(DisplayOp)]
pub fn enum_derive_display_op(input: TokenStream) -> TokenStream {
    let DeriveInput { ident, data, .. } = parse_macro_input!(input);

    if let Data::Enum(e) = data {
        let mut fmt_dsts_cases = TokenStream2::new();
        let mut fmt_op_cases = TokenStream2::new();
        for v in e.variants {
            let case = v.ident;
            fmt_dsts_cases.extend(quote! {
                #ident::#case(x) => x.fmt_dsts(f),
            });
            fmt_op_cases.extend(quote! {
                #ident::#case(x) => x.fmt_op(f),
            });
        }
        quote! {
            impl DisplayOp for #ident {
                fn fmt_dsts(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #fmt_dsts_cases
                    }
                }

                fn fmt_op(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                    match self {
                        #fmt_op_cases
                    }
                }
            }
        }
        .into()
    } else {
        panic!("Not an enum type");
    }
}

#[proc_macro_derive(FromVariants)]
pub fn derive_from_variants(input: TokenStream) -> TokenStream {
    let DeriveInput { ident, data, .. } = parse_macro_input!(input);
    let enum_type = ident;

    let mut impls = TokenStream2::new();

    if let Data::Enum(e) = data {
        for v in e.variants {
            let var_ident = v.ident;
            let from_type = match v.fields {
                Fields::Unnamed(FieldsUnnamed { unnamed, .. }) => unnamed,
                _ => panic!("Expected Op(OpFoo)"),
            };

            let quote = quote! {
                impl From<#from_type> for #enum_type {
                    fn from (op: #from_type) -> #enum_type {
                        #enum_type::#var_ident(op)
                    }
                }
            };

            impls.extend(quote);
        }
    }

    impls.into()
}
