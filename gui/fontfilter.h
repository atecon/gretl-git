/* 
 *  gretl -- Gnu Regression, Econometrics and Time-series Library
 *  Copyright (C) 2001 Allin Cottrell and Riccardo "Jack" Lucchetti
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#ifndef GRETL_FONTFILTER_H
#define GRETL_FONTFILTER_H

typedef enum {
    FONT_FILTER_NONE,
    FONT_FILTER_LATIN,
    FONT_FILTER_LATIN_MONO
} GretlFontFilter;   

int gretl_font_filter_init (void);

void gretl_font_filter_cleanup (void);

int validate_font_family (PangoFontFamily *family,
			  const gchar *famname, 
			  gint i, gint nf,
			  gint filter, gint *err);

int validate_single_font (const PangoFontFamily *family,
			  gint filter);

#endif /* GRETL_FONTFILTER_H */
