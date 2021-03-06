//------------------------------------------------------------------------------
// <copyright file="StatusStripPanelBorderSide.cs" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>                                                                
//------------------------------------------------------------------------------

namespace System.Windows.Forms {
    using System;
    using System.Windows.Forms;
    using System.ComponentModel;
    using System.Drawing.Design;
    
    [System.Runtime.InteropServices.ComVisible(true), 
    Editor("System.Windows.Forms.Design.BorderSidesEditor, " + AssemblyRef.SystemDesign, typeof(UITypeEditor)),
    Flags]
    public enum ToolStripStatusLabelBorderSides {
        All = Border3DSide.Top | Border3DSide.Bottom | Border3DSide.Left | Border3DSide.Right, // not mapped to Border3DSide.All because we NEVER want to fill the middle. 
        Bottom = Border3DSide.Bottom, 
        Left = Border3DSide.Left, 
        Right = Border3DSide.Right, 
        Top = Border3DSide.Top,
        None = 0
    } 
}

