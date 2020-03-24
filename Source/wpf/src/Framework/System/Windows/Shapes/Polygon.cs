//---------------------------------------------------------------------------
// File: Polygon.cs
//
// Description:
// Implementation of Polygon shape element.
//
// History:
//  05/30/02 - Microsoft - Created.
//
// Copyright (C) 2003 by Microsoft Corporation.  All rights reserved.
//
//---------------------------------------------------------------------------
using System.Windows.Shapes;
using System.Diagnostics;
using System.Windows.Threading;

using System.Windows;
using System.Windows.Media;
using MS.Internal;

using System;

namespace System.Windows.Shapes
{
    /// <summary>
    /// The polygon shape element
    /// This element (like all shapes) belongs under a Canvas,
    /// and will be presented by the parent canvas.
    /// Since a Polygon is really a polyline which closes its path
    /// </summary>
    public sealed class Polygon : Shape
    {

        #region Constructors
        
        /// <summary>
        /// Instantiates a new instance of a polygon.
        /// </summary>
        public Polygon()
        {
        }

        #endregion Constructors

        #region Dynamic Properties

        /// <summary>
        /// Points property
        /// </summary>
        public static readonly DependencyProperty PointsProperty = DependencyProperty.Register(
                "Points", typeof(PointCollection), typeof(Polygon), 
                new FrameworkPropertyMetadata(new FreezableDefaultValueFactory(PointCollection.Empty), FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender));

        /// <summary>
        /// Points property
        /// </summary>
        public PointCollection Points
        {
            get
            {
                return (PointCollection)GetValue(PointsProperty);
            }
            set
            {
                SetValue(PointsProperty, value);
            }
        }

        /// <summary>
        /// FillRule property
        /// </summary>
        public static readonly DependencyProperty FillRuleProperty = DependencyProperty.Register(
            "FillRule", 
            typeof(FillRule), 
            typeof(Polygon),
            new FrameworkPropertyMetadata(
                FillRule.EvenOdd, 
                FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender),
            new ValidateValueCallback(System.Windows.Media.ValidateEnums.IsFillRuleValid)
            );

        /// <summary>
        /// FillRule property
        /// </summary>
        public FillRule FillRule
        {
            get
            {
                return (FillRule)GetValue(FillRuleProperty);
            }
            set
            {
                SetValue(FillRuleProperty, value);
            }
        }
        #endregion Dynamic Properties

        #region Protected Methods and properties
        
        /// <summary>
        /// Get the polygon that defines this shape
        /// </summary>
        protected override Geometry DefiningGeometry
        {
            get
            {
                return _polygonGeometry;
            }
        }

        #endregion


        #region Internal Methods
        internal override void CacheDefiningGeometry()
        {
            PointCollection pointCollection = Points;
            PathFigure pathFigure = new PathFigure();

            // Are we degenerate?
            // Yes, if we don't have data
            if (pointCollection == null)
            {
                _polygonGeometry = Geometry.Empty;
                return;
            }

            // Create the polygon PathGeometry
            // ISSUE-Microsoft-07/11/2003 - 


            if (pointCollection.Count > 0)
            {
                pathFigure.StartPoint = pointCollection[0];

                if (pointCollection.Count > 1)
                {
                    Point[] array = new Point[pointCollection.Count - 1];

                    for (int i = 1; i < pointCollection.Count; i++)
                    {
                        array[i - 1] = pointCollection[i];
                    }

                    pathFigure.Segments.Add(new PolyLineSegment(array, true));
                }

                pathFigure.IsClosed = true;
            }

            PathGeometry polygonGeometry = new PathGeometry();
            polygonGeometry.Figures.Add(pathFigure);

            // Set FillRule
            polygonGeometry.FillRule = FillRule;

            _polygonGeometry = polygonGeometry;
        }
        #endregion Internal Methods


        #region Private Methods and Members

        private Geometry _polygonGeometry;

        #endregion
   }
}