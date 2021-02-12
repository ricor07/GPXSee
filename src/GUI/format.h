#ifndef FORMAT_H
#define FORMAT_H

#include <QString>
#include "units.h"

class Coordinates;

enum CoordinatesFormat {
	DecimalDegrees,
	DegreesMinutes,
	DMS
};

namespace Format
{
	QString timeSpan(qreal time, bool full = true);
	QString distance(qreal value, Units units);
	QString elevation(qreal value, Units units);
	QString coordinates(const Coordinates &c, CoordinatesFormat type);
	QString lon(const Coordinates &c, CoordinatesFormat type);
	QString lat(const Coordinates &c, CoordinatesFormat type);
}

#endif // FORMAT_H
