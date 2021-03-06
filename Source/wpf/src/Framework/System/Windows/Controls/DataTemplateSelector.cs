//---------------------------------------------------------------------------
//
// <copyright file="DataTemplateSelector.cs" company="Microsoft">
//    Copyright (C) Microsoft Corporation.  All rights reserved.
// </copyright>
//
// Description: DataTemplateSelector allows the app writer to provide custom template selection logic.
//
// Specs:       http://avalon/coreui/Specs%20%20Property%20Engine/Styling%20Revisited.doc
//
//---------------------------------------------------------------------------

namespace System.Windows.Controls
{
    /// <summary>
    /// <p>
    /// DataTemplateSelector allows the app writer to provide custom template selection logic.
    /// For example, with a class 








    public class DataTemplateSelector
    {
        /// <summary>
        /// Override this method to return an app specific <seealso cref="DataTemplate"/>.
        /// </summary>
        /// <param name="item">The data content</param>
        /// <param name="container">The element to which the template will be applied</param>
        /// <returns>an app-specific template to apply, or null.</returns>
        public virtual DataTemplate SelectTemplate(object item, DependencyObject container)
        {
            return null;
        }
    }
}
