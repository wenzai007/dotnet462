//---------------------------------------------------------------------------
// 
// File: XamlAttribute.cs
//
// Copyright (C) Microsoft Corporation.  All rights reserved.
//
// Description: Xaml attribute enumeration.
//
//---------------------------------------------------------------------------

namespace System.Windows.Documents
{
    /// <summary>
    /// Xaml attribute enumeration that will be converted from Xaml to Rtf
    /// property control.
    /// </summary>
    internal enum XamlAttribute
    {
        XAUnknown,
        XAFontWeight,
        XAFontSize,
        XAFontStyle,
        XAFontFamily,
        XAFontStretch,
        XABackground,
        XAForeground,
        XAFlowDirection,
        XATextDecorations,
        XATextAlignment,
        XAMarkerStyle,
        XATextIndent,
        XAColumnSpan,
        XARowSpan,
        XAStartIndex,
        XAMarkerOffset,
        XABorderThickness,
        XABorderBrush,
        XAPadding,
        XAMargin,
        XAKeepTogether,
        XAKeepWithNext,
        XABaselineAlignment,
        XABaselineOffset,
        XANavigateUri,
        XATargetName,
        XALineHeight,
        XALocation,
        XAWidth,
        XAHeight,
        XASource,
        XAUriSource,
        XAStretch,
        XAStretchDirection,
        XACellSpacing,
        XATypographyVariants,
        XALang
    }
}
